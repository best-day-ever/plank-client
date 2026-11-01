#include "internetconnectivity.h"

#include <QDateTime>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QtDebug>

// One bounded asynchronous ping, shared by the ordered background check and
// the on-demand concurrent summary. Cancellation reaps the child before reuse.
struct InternetConnectivity::Probe : QObject
{
    using Completion = std::function<void(Probe*, State, double)>;

    Probe(QObject* parent, PingCommand command, int timeoutMs, Completion completion)
        : QObject(parent), command(std::move(command)), completion(std::move(completion)),
          process(this)
    {
        deadline.setSingleShot(true);
        deadline.setTimerType(Qt::PreciseTimer);
        deadline.setInterval(timeoutMs);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        process.setProcessEnvironment(environment);
        process.setStandardInputFile(QProcess::nullDevice());
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] { drain(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] { drain(); });
        connect(&deadline, &QTimer::timeout, this, [this] {
            timedOut = true;
            process.kill();
        });
        connect(&process, &QProcess::started, this, [this] {
            if (cancelled || timedOut) process.kill();
        });
        connect(&process, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) { finish(code, status); });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(-1, QProcess::CrashExit);
            // Killed/crashed children also emit finished; consume them once.
        });
    }

    ~Probe() override
    {
        // The parent monitor may already be in QObject destruction. Do not call
        // its captured completion while QProcess reaps a child during shutdown.
        process.disconnect(this);
        process.kill();
    }

    void start()
    {
        if (cancelled) { finish(-1, QProcess::CrashExit); return; }
        deadline.start();
        process.start(command.program, command.arguments);
    }

    void cancel()
    {
        cancelled = true;
        deadline.stop();
        process.kill();
    }

    void drain()
    {
        if (!process.isOpen()) return;
        output.append(process.readAllStandardOutput().left(4096 - output.size()));
        error.append(process.readAllStandardError().left(1024 - error.size()));
    }

    double roundTripMs() const
    {
        // -q -c 1 emits this summary on both iputils and macOS ping. Parse the
        // actual ICMP RTT, never process startup/exit time. Success without a
        // recognized timing line remains reachable with no RTT available.
        static const QRegularExpression summary(QStringLiteral(
            R"((?:^|\n)(?:rtt|round-trip) min/avg/max/(?:mdev|stddev) = [0-9]+(?:\.[0-9]+)?/([0-9]+(?:\.[0-9]+)?)/[0-9]+(?:\.[0-9]+)?/[0-9]+(?:\.[0-9]+)? ms(?:\r?\n|$))"));
        const auto match = summary.match(QString::fromLatin1(output));
        if (!match.hasMatch()) return -1;
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        return ok && qIsFinite(value) && value >= 0 ? value : -1;
    }

    void finish(int exitCode, QProcess::ExitStatus exitStatus)
    {
        if (finished) return;
        finished = true;
        deadline.stop();
        drain();
        State state = NoResponse;
        if (!cancelled && !timedOut) {
            const bool routeUnavailable = error.contains("Network is unreachable") ||
                    error.contains("No route to host") || error.contains("Network is down") ||
                    error.contains("Host is down") || error.contains("Host is unreachable");
            if (exitStatus == QProcess::NormalExit && exitCode == 0) state = Reachable;
            else if (exitStatus != QProcess::NormalExit ||
                     (exitCode != command.noResponseExitCode && !routeUnavailable)) state = Unavailable;
        }
        const double rtt = state == Reachable ? roundTripMs() : -1;
        const auto notify = completion;
        deleteLater();
        notify(this, state, rtt);
    }

    const PingCommand command;
    const Completion completion;
    QProcess process;
    QTimer deadline;
    QByteArray output;
    QByteArray error;
    bool cancelled = false;
    bool timedOut = false;
    bool finished = false;
};

InternetConnectivity::InternetConnectivity(const PlankClientPolicy::InternetCheck& settings,
                                           QObject* parent, CommandFactory commandFactory)
    : QObject(parent), m_Settings(settings), m_CommandFactory(std::move(commandFactory)),
      m_ConfigurationError(settings.error)
{
    // Numeric unicast addresses avoid DNS timeouts and command/option injection.
    // Do not fall back to public destinations if an explicit list is invalid.
    if (settings.hosts.isEmpty() || settings.hosts.size() > 8) {
        m_ConfigurationError = tr("Specify between 1 and 8 Internet-check IP addresses.");
    }
    for (const auto& host : settings.hosts) {
        const QHostAddress address(host);
        if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isMulticast() ||
                address == QHostAddress::Broadcast || address == QHostAddress::AnyIPv4) {
            m_ConfigurationError = tr("Internet-check targets must be unicast IPv4 addresses.");
        }
    }
    if (!settings.enabled) m_State = Disabled;
    else if (!m_ConfigurationError.isEmpty()) {
        m_State = Unavailable;
        m_Detail = m_ConfigurationError;
    }
    else m_Detail = tr("Checking the configured Internet addresses. PLANK connections remain available.");

    m_Poll.setSingleShot(true);
    m_NetworkChange.setSingleShot(true);
    connect(&m_Poll, &QTimer::timeout, this, &InternetConnectivity::checkNow);
    connect(&m_NetworkChange, &QTimer::timeout, this, &InternetConnectivity::checkNow);
}

InternetConnectivity::~InternetConnectivity()
{
    stop();
}

QString InternetConnectivity::statusText() const
{
    switch (m_State) {
    case Disabled: return tr("Internet: Disabled");
    case Checking: return tr("Internet: Checking");
    case Reachable: return tr("Internet: Reachable");
    case NoResponse: return tr("Internet: No response");
    case Unavailable: return tr("Internet: Unavailable");
    }
    return {};
}

InternetConnectivity::PingCommand InternetConnectivity::systemPingCommand(const QString& host, int timeoutMs)
{
    const QHostAddress address(host);
    const QString deadline = QString::number((timeoutMs + 999) / 1000);
#ifdef Q_OS_MACOS
    return {QStringLiteral("/sbin/ping"),
            {QStringLiteral("-n"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("1"),
             QStringLiteral("-t"), deadline, address.toString()}, 2};
#elif defined(Q_OS_LINUX)
    return {QStringLiteral("/usr/bin/ping"),
            {QStringLiteral("-4"), QStringLiteral("-n"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("1"),
             QStringLiteral("-w"), deadline, address.toString()}, 1};
#else
    Q_UNUSED(deadline)
    return {};
#endif
}

void InternetConnectivity::start()
{
    if (m_Running || !m_Settings.enabled) return;
    if (!m_ConfigurationError.isEmpty()) {
        qWarning() << "Internet connectivity check unavailable:" << m_ConfigurationError;
        return;
    }
    m_Running = true;
    checkNow();
}

void InternetConnectivity::stop()
{
    m_Running = false;
    m_RestartPending = false;
    m_Poll.stop();
    m_NetworkChange.stop();
    if (m_Probe) m_Probe->cancel();
    cancelSummary();
}

void InternetConnectivity::networkChanged()
{
    // Coalesce bursts of platform notifications. Neither OS reachability nor
    // transport type is itself proof that a configured destination responds.
    if (m_Running) m_NetworkChange.start(250);
}

void InternetConnectivity::refreshIfStale()
{
    // Wall time includes suspend; activation need not generate another ping if
    // the normal periodic check has just run following resume.
    if (m_Running && QDateTime::currentMSecsSinceEpoch() - m_LastStartedMs >= m_Settings.intervalMs)
        checkNow();
}

void InternetConnectivity::checkNow()
{
    if (!m_Running) return;
    m_Poll.stop();
    if (m_Probe) {
        m_RestartPending = true;
        m_Probe->cancel();
        return; // Reap this child before starting another; no overlapping rounds.
    }
    beginRound();
}

void InternetConnectivity::beginRound()
{
    m_LastStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_RestartPending = false;
    m_ProbeError = false;
    m_Target = 0;
    startTarget();
}

void InternetConnectivity::startTarget()
{
    m_Probe = new Probe(this, m_CommandFactory(m_Settings.hosts.at(m_Target), m_Settings.timeoutMs),
                        m_Settings.timeoutMs, [this](Probe* probe, State state, double) {
        targetFinished(probe, state);
    });
    m_Probe->start();
}

void InternetConnectivity::targetFinished(Probe* probe, State state)
{
    if (probe != m_Probe) return;
    m_Probe = nullptr;
    if (!m_Running) return;
    if (m_RestartPending) {
        beginRound();
        return;
    }
    if (state == Reachable) {
        completeRound(Reachable, tr("Reply received from %1. This checks access to a configured address, "
                                   "not DNS, VPN connectivity or a particular PLANK Host.")
                      .arg(m_Settings.hosts.at(m_Target)));
        return;
    }
    if (state == Unavailable) m_ProbeError = true;
    if (++m_Target < m_Settings.hosts.size()) {
        startTarget();
        return;
    }
    if (m_ProbeError) {
        completeRound(Unavailable, tr("One or more Internet checks could not run. "
                                      "Check the system ping utility and client.conf. PLANK connections remain available."));
    }
    else {
        completeRound(NoResponse, tr("None of the configured Internet addresses replied. "
                                     "Your connection may be unavailable, or the network may block ping. "
                                     "PLANK connections remain available."));
    }
}

QVariantList InternetConnectivity::summaryResults() const
{
    QVariantList results;
    for (const auto& row : m_SummaryRows) {
        QString text;
        switch (row.state) {
        case Checking: text = tr("Checking"); break;
        case Reachable: text = tr("Reachable"); break;
        case NoResponse: text = tr("No response"); break;
        case Unavailable: text = tr("Unavailable"); break;
        case Disabled: text = tr("Disabled"); break;
        }
        results.append(QVariantMap{{QStringLiteral("address"), row.address},
                                  {QStringLiteral("state"), int(row.state)},
                                  {QStringLiteral("status"), text},
                                  {QStringLiteral("roundTripMs"), row.roundTripMs}});
    }
    return results;
}

QString InternetConnectivity::summaryCheckedAt() const
{
    return m_SummaryCompletedAt.isValid() ?
                m_SummaryCompletedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
}

void InternetConnectivity::refreshSummary()
{
    if (!m_Settings.enabled || !m_ConfigurationError.isEmpty()) {
        m_SummaryError = !m_Settings.enabled ? tr("Internet checks are disabled in client.conf.") : m_ConfigurationError;
        emit summaryChanged();
        return;
    }
    if (!m_SummaryProbes.isEmpty()) {
        m_SummaryRestartPending = true;
        m_SummaryCancelled = true;
        const auto probes = m_SummaryProbes;
        for (auto* probe : probes) probe->cancel();
        return; // Coalesce rapid reopen/refresh until every old child is reaped.
    }
    m_SummaryRestartPending = false;
    m_SummaryCancelled = false;
    m_SummaryCompletedAt = {};
    m_SummaryRows.clear();
    for (const auto& address : m_Settings.hosts) {
        const int row = m_SummaryRows.size();
        m_SummaryRows.append(SummaryResult{address, Checking, -1});
        m_SummaryProbes.append(new Probe(this, m_CommandFactory(address, m_Settings.timeoutMs),
                                        m_Settings.timeoutMs, [this, row](Probe* probe, State state, double rtt) {
            summaryFinished(probe, row, state, rtt);
        }));
    }
    emit summaryChanged();
    // Copy the list because FailedToStart/finished may remove its own probe.
    const auto probes = m_SummaryProbes;
    for (auto* probe : probes) probe->start();
}

void InternetConnectivity::cancelSummary()
{
    m_SummaryRestartPending = false;
    m_SummaryCancelled = true;
    const auto probes = m_SummaryProbes;
    for (auto* probe : probes) probe->cancel();
}

void InternetConnectivity::summaryFinished(Probe* probe, int row, State state, double roundTripMs)
{
    if (!m_SummaryProbes.removeOne(probe)) return;
    if (!m_SummaryCancelled) {
        m_SummaryRows[row].state = state;
        m_SummaryRows[row].roundTripMs = roundTripMs;
    }
    if (m_SummaryProbes.isEmpty()) {
        if (m_SummaryRestartPending) { refreshSummary(); return; }
        if (!m_SummaryCancelled) m_SummaryCompletedAt = QDateTime::currentDateTime();
    }
    emit summaryChanged();
}

void InternetConnectivity::completeRound(State state, const QString& detail)
{
    setState(state, detail);
    // Start-to-start cadence, without concurrent rounds or accumulating work.
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_LastStartedMs;
    const qint64 interval = m_Settings.intervalMs;
    m_Poll.start(static_cast<int>(qBound(qint64(1000), interval - elapsed, interval)));
}

void InternetConnectivity::setState(State state, const QString& detail)
{
    if (state == m_State && detail == m_Detail) return;
    if (state != m_State) qInfo() << "Internet connectivity:" << state;
    m_State = state;
    m_Detail = detail;
    emit stateChanged();
}

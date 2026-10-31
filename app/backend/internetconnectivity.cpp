#include "internetconnectivity.h"

#include <QDateTime>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QtDebug>

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

    m_Deadline.setSingleShot(true);
    m_Deadline.setTimerType(Qt::PreciseTimer);
    m_Poll.setSingleShot(true);
    m_NetworkChange.setSingleShot(true);
    connect(&m_Deadline, &QTimer::timeout, this, [this] {
        if (m_Process) {
            m_TimedOut = true;
            m_Process->kill();
        }
    });
    connect(&m_Poll, &QTimer::timeout, this, &InternetConnectivity::checkNow);
    connect(&m_NetworkChange, &QTimer::timeout, this, &InternetConnectivity::checkNow);
}

InternetConnectivity::~InternetConnectivity()
{
    stop();
    if (m_Process) m_Process->disconnect(this);
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
    m_Deadline.stop();
    if (m_Process) m_Process->kill();
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
    if (m_Process) {
        m_RestartPending = true;
        m_Deadline.stop();
        m_Process->kill();
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
    const auto command = m_CommandFactory(m_Settings.hosts.at(m_Target), m_Settings.timeoutMs);
    auto* process = new QProcess(this);
    m_Process = process;
    m_NoResponseExitCode = command.noResponseExitCode;
    m_TimedOut = false;
    m_ProbeStderr.clear();
    // Success comes from exit status. Keep only bounded diagnostics to distinguish
    // no route/no network from a broken utility or denied ICMP permission. iputils
    // uses the same error exit code for both cases. Normalize utility diagnostics.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardInputFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const auto output = process->readAllStandardError();
        if (process == m_Process) m_ProbeStderr.append(output.left(1024 - m_ProbeStderr.size()));
    });
    connect(process, &QProcess::started, this, [this, process] {
        if (process == m_Process && (!m_Running || m_RestartPending || m_TimedOut)) process->kill();
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        targetFinished(process, code, status);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) targetFinished(process, -1, QProcess::CrashExit);
        // A killed/crashed child also emits finished; consume it exactly once.
    });
    m_Deadline.start(m_Settings.timeoutMs);
    process->start(command.program, command.arguments);
}

void InternetConnectivity::targetFinished(QProcess* process, int exitCode, QProcess::ExitStatus exitStatus)
{
    if (process != m_Process) return;
    m_Deadline.stop();
    m_Process = nullptr;
    process->deleteLater();
    if (!m_Running) return;
    if (m_RestartPending) {
        beginRound();
        return;
    }
    if (!m_TimedOut && exitStatus == QProcess::NormalExit && exitCode == 0) {
        completeRound(Reachable, tr("Reply received from %1. This checks access to a configured address, "
                                   "not DNS, VPN connectivity or a particular PLANK Host.")
                      .arg(m_Settings.hosts.at(m_Target)));
        return;
    }
    if (process->isOpen())
        m_ProbeStderr.append(process->readAllStandardError().left(1024 - m_ProbeStderr.size()));
    const bool routeUnavailable = m_ProbeStderr.contains("Network is unreachable") ||
            m_ProbeStderr.contains("No route to host") || m_ProbeStderr.contains("Network is down") ||
            m_ProbeStderr.contains("Host is down") || m_ProbeStderr.contains("Host is unreachable");
    if (!m_TimedOut && (exitStatus != QProcess::NormalExit ||
                       (exitCode != m_NoResponseExitCode && !routeUnavailable)))
        m_ProbeError = true;
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

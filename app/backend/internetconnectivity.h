#pragma once

#include "../settings/plankclientpolicy.h"

#include <QObject>
#include <QDateTime>
#include <QProcess>
#include <QTimer>
#include <QVariantList>
#include <functional>

// Informational only. This monitor has no access to bookmarks, authentication,
// Host polling or session recovery; a failed ping can never gate a connection.
class InternetConnectivity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)
    Q_PROPERTY(QVariantList summaryResults READ summaryResults NOTIFY summaryChanged)
    Q_PROPERTY(bool summaryRunning READ summaryRunning NOTIFY summaryChanged)
    Q_PROPERTY(QString summaryCheckedAt READ summaryCheckedAt NOTIFY summaryChanged)
    Q_PROPERTY(QString summaryError READ summaryError NOTIFY summaryChanged)

public:
    enum State { Disabled, Checking, Reachable, NoResponse, Unavailable };
    Q_ENUM(State)

    struct PingCommand {
        QString program;
        QStringList arguments;
        int noResponseExitCode = 1;
        // Windows can exit zero for an ICMP error. Require an actual echo
        // reply instead of interpreting its localized human-readable status.
        bool requiresWindowsEchoReply = false;
    };
    using CommandFactory = std::function<PingCommand(const QString&, int)>;

    explicit InternetConnectivity(const PlankClientPolicy::InternetCheck& settings,
                                  QObject* parent = nullptr,
                                  CommandFactory commandFactory = systemPingCommand);
    ~InternetConnectivity() override;

    State state() const { return m_State; }
    QString statusText() const;
    QString detail() const { return m_Detail; }
    static PingCommand systemPingCommand(const QString& address, int timeoutMs);

    void start();
    void stop();
    void checkNow();
    void networkChanged();
    void refreshIfStale();

    QVariantList summaryResults() const;
    bool summaryRunning() const { return !m_SummaryProbes.isEmpty(); }
    QString summaryCheckedAt() const;
    QString summaryError() const { return m_SummaryError; }
    Q_INVOKABLE void refreshSummary();
    Q_INVOKABLE void cancelSummary();

signals:
    void stateChanged();
    void summaryChanged();

private:
    struct Probe;
    struct SummaryResult {
        QString address;
        State state = Checking;
        double roundTripMs = -1;
    };
    void beginRound();
    void startTarget();
    void targetFinished(Probe* probe, State state);
    void summaryFinished(Probe* probe, int row, State state, double roundTripMs);
    void completeRound(State state, const QString& detail);
    void setState(State state, const QString& detail);

    const PlankClientPolicy::InternetCheck m_Settings;
    const CommandFactory m_CommandFactory;
    Probe* m_Probe = nullptr;
    QTimer m_Poll;
    QTimer m_NetworkChange;
    State m_State = Checking;
    QString m_Detail;
    int m_Target = 0;
    qint64 m_LastStartedMs = 0;
    bool m_Running = false;
    bool m_RestartPending = false;
    bool m_ProbeError = false;
    QString m_ConfigurationError;
    QList<Probe*> m_SummaryProbes;
    QList<SummaryResult> m_SummaryRows;
    QDateTime m_SummaryCompletedAt;
    QString m_SummaryError;
    bool m_SummaryCancelled = false;
    bool m_SummaryRestartPending = false;
};

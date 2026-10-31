#pragma once

#include "../settings/plankclientpolicy.h"

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

// Informational only. This monitor has no access to bookmarks, authentication,
// Host polling or session recovery; a failed ping can never gate a connection.
class InternetConnectivity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString detail READ detail NOTIFY stateChanged)

public:
    enum State { Disabled, Checking, Reachable, NoResponse, Unavailable };
    Q_ENUM(State)

    struct PingCommand {
        QString program;
        QStringList arguments;
        int noResponseExitCode = 1;
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

signals:
    void stateChanged();

private:
    void beginRound();
    void startTarget();
    void targetFinished(QProcess* process, int exitCode, QProcess::ExitStatus exitStatus);
    void completeRound(State state, const QString& detail);
    void setState(State state, const QString& detail);

    const PlankClientPolicy::InternetCheck m_Settings;
    const CommandFactory m_CommandFactory;
    QProcess* m_Process = nullptr;
    QTimer m_Deadline;
    QTimer m_Poll;
    QTimer m_NetworkChange;
    State m_State = Checking;
    QString m_Detail;
    int m_Target = 0;
    int m_NoResponseExitCode = 1;
    qint64 m_LastStartedMs = 0;
    bool m_Running = false;
    bool m_RestartPending = false;
    bool m_TimedOut = false;
    bool m_ProbeError = false;
    QByteArray m_ProbeStderr;
    QString m_ConfigurationError;
};

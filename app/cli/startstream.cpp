#include "startstream.h"
#include "backend/computermanager.h"
#include "backend/computerseeker.h"
#include "streaming/session.h"

#include <QCoreApplication>
#include <QTimer>

#define COMPUTER_SEEK_TIMEOUT 30000
#define APP_SEEK_TIMEOUT 10000

namespace CliStartStream
{

enum State {
    StateInit,
    StateSeekComputer,
    StateAuthenticate,
    StateSeekApp,
    StateStartSession,
    StateFailure,
};

class Event
{
public:
    enum Type {
        AuthenticationCompleted,
        ComputerFound,
        ComputerUpdated,
        Executed,
        Timedout,
    };

    Event(Type type)
        : type(type), computerManager(nullptr), computer(nullptr) {}

    Type type;
    ComputerManager *computerManager;
    NvComputer *computer;
    QString errorMessage;
};

class LauncherPrivate
{
    Q_DECLARE_PUBLIC(Launcher)

public:
    LauncherPrivate(Launcher *q) : q_ptr(q) {}

    void handleEvent(Event event)
    {
        Q_Q(Launcher);
        Session* session;
        NvApp app;

        switch (event.type) {
        // Occurs when CliStartStreamSegue becomes visible and the UI calls launcher's execute()
        case Event::Executed:
            if (m_State == StateInit) {
                m_State = StateSeekComputer;
                m_ComputerManager = event.computerManager;

                m_ComputerSeeker = new ComputerSeeker(m_ComputerManager, m_ComputerName, q);
                q->connect(m_ComputerSeeker, &ComputerSeeker::computerFound,
                           q, &Launcher::onComputerFound);
                q->connect(m_ComputerSeeker, &ComputerSeeker::errorTimeout,
                           q, &Launcher::onTimeout);
                m_ComputerSeeker->start(COMPUTER_SEEK_TIMEOUT);

                q->connect(m_ComputerManager, &ComputerManager::computerStateChanged,
                           q, &Launcher::onComputerUpdated);
                q->connect(m_ComputerManager, &ComputerManager::authenticationCompleted,
                           q, &Launcher::onAuthenticationCompleted);

                emit q->searchingComputer();
            }
            break;
        // Occurs when searched computer is found
        case Event::ComputerFound:
            if (m_State == StateSeekComputer) {
                if (event.computer->authorizationState == NvComputer::AS_AUTHORIZED) {
                    beginAppLookup(event.computer);
                } else if (event.computer->stationConnectAuthentication &&
                           !m_StationConnectUsername.isEmpty() &&
                           !m_StationConnectPassword.isEmpty()) {
                    m_State = StateAuthenticate;
                    m_Computer = event.computer;
                    m_ComputerManager->authenticateHost(
                            m_Computer, m_StationConnectUsername,
                            std::move(m_StationConnectPassword));
                    m_StationConnectPassword.clear();
                    emit q->authenticatingComputer();
                } else {
                    m_State = StateFailure;
                    QString msg;
                    if (event.computer->stationConnectAuthentication) {
                        msg = QObject::tr("Computer %1 requires StationConnect credentials. "
                                          "Use --stationconnect-user with "
                                          "--stationconnect-password-stdin.")
                                .arg(event.computer->name);
                    } else {
                        msg = QObject::tr("Computer %1 requires StationConnect sign-in before streaming.")
                                .arg(event.computer->name);
                    }
                    emit q->failed(msg);
                }
            }
            break;
        case Event::AuthenticationCompleted:
            if (m_State == StateAuthenticate && event.computer == m_Computer) {
                if (event.errorMessage.isEmpty()) {
                    beginAppLookup(event.computer);
                } else {
                    m_State = StateFailure;
                    emit q->failed(event.errorMessage);
                }
            }
            break;
        // Occurs when a computer is updated
        case Event::ComputerUpdated:
            if (m_State == StateSeekApp) {
                int index = getAppIndex();
                if (-1 != index) {
                    app = m_Computer->appList[index];
                    m_TimeoutTimer->stop();
                    if (isNotStreaming() || isStreamingApp(app)) {
                        m_State = StateStartSession;
                        session = new Session(m_Computer, app, m_Preferences,
                                              m_ComputerManager);
                        emit q->sessionCreated(app.name, session);
                    } else {
                        m_State = StateFailure;
                        emit q->failed(QObject::tr("A different host application is already running."));
                    }
                }
            }
            break;
        // Occurs when computer or app search timed out
        case Event::Timedout:
            if (m_State == StateSeekComputer) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to connect to %1").arg(m_ComputerName));
            }
            if (m_State == StateSeekApp) {
                m_State = StateFailure;
                emit q->failed(QObject::tr("Failed to find application %1").arg(m_AppName));
            }
            break;
        }
    }

    int getAppIndex() const
    {
        for (int i = 0; i < m_Computer->appList.length(); i++) {
            if (m_Computer->appList[i].name.toLower() == m_AppName.toLower()) {
                return i;
            }
        }
        return -1;
    }

    void beginAppLookup(NvComputer *computer)
    {
        Q_Q(Launcher);
        m_State = StateSeekApp;
        m_Computer = computer;
        m_TimeoutTimer->start(APP_SEEK_TIMEOUT);
        emit q->searchingApp();

        Event event(Event::ComputerUpdated);
        event.computer = computer;
        handleEvent(event);
    }

    bool isNotStreaming() const
    {
        return m_Computer->currentGameId == 0;
    }

    bool isStreamingApp(NvApp app) const
    {
        return m_Computer->currentGameId == app.id;
    }

    QString getCurrentAppName() const
    {
        for (const NvApp& app : m_Computer->appList) {
            if (m_Computer->currentGameId == app.id) {
                return app.name;
            }
        }
        return "<UNKNOWN>";
    }

    Launcher *q_ptr;
    QString m_ComputerName;
    QString m_AppName;
    QString m_StationConnectUsername;
    QString m_StationConnectPassword;
    StreamingPreferences *m_Preferences;
    ComputerManager *m_ComputerManager;
    ComputerSeeker *m_ComputerSeeker;
    NvComputer *m_Computer;
    State m_State;
    QTimer *m_TimeoutTimer;
};

Launcher::Launcher(QString computer, QString app,
                   StreamingPreferences* preferences,
                   QString stationConnectUsername,
                   QString stationConnectPassword,
                   QObject *parent)
    : QObject(parent),
      m_DPtr(new LauncherPrivate(this))
{
    Q_D(Launcher);
    d->m_ComputerName = computer;
    d->m_AppName = app;
    d->m_StationConnectUsername = std::move(stationConnectUsername);
    d->m_StationConnectPassword = std::move(stationConnectPassword);
    d->m_Preferences = preferences;
    d->m_State = StateInit;
    d->m_TimeoutTimer = new QTimer(this);
    d->m_TimeoutTimer->setSingleShot(true);
    connect(d->m_TimeoutTimer, &QTimer::timeout,
            this, &Launcher::onTimeout);
}

Launcher::~Launcher()
{
    Q_D(Launcher);
    d->m_StationConnectPassword.fill(QChar('\0'));
    d->m_StationConnectPassword.clear();
}

void Launcher::execute(ComputerManager *manager)
{
    Q_D(Launcher);
    Event event(Event::Executed);
    event.computerManager = manager;
    d->handleEvent(event);
}

bool Launcher::isExecuted() const
{
    Q_D(const Launcher);
    return d->m_State != StateInit;
}

void Launcher::onComputerFound(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerFound);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onComputerUpdated(NvComputer *computer)
{
    Q_D(Launcher);
    Event event(Event::ComputerUpdated);
    event.computer = computer;
    d->handleEvent(event);
}

void Launcher::onAuthenticationCompleted(NvComputer *computer, QString error)
{
    Q_D(Launcher);
    Event event(Event::AuthenticationCompleted);
    event.computer = computer;
    event.errorMessage = std::move(error);
    d->handleEvent(event);
}

void Launcher::onTimeout()
{
    Q_D(Launcher);
    Event event(Event::Timedout);
    d->handleEvent(event);
}

}

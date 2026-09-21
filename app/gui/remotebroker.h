#pragma once

#include "backend/plankbroker.h"
#include "backend/plankbrokerclient.h"

#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class ComputerManager;
class NvComputer;
class Session;
class StreamingPreferences;

// "Remote (broker)" mode controller exposed to QML as a singleton. Owns the
// in-memory broker session token (never persisted), the host list, brokered
// connects (bde-linux docs/plank-broker.md section 10.2) and the lease
// keepalive while a brokered stream runs.
class RemoteBroker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString brokerAddress READ brokerAddress NOTIFY configurationChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY configurationChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString busyText READ busyText NOTIFY stateChanged)
    Q_PROPERTY(QString username READ username NOTIFY stateChanged)
    Q_PROPERTY(QVariantList hosts READ hosts NOTIFY hostsChanged)

public:
    explicit RemoteBroker(StreamingPreferences* preferences, QObject* parent = nullptr);
    ~RemoteBroker() override;

    Q_INVOKABLE void initialize(ComputerManager* computerManager);
    Q_INVOKABLE void signIn(const QString& username, QString password, QString otp);
    Q_INVOKABLE void refreshHosts();
    // Connects with the saved display setup; without one (or when the saved
    // "match my displays" no longer fits the current screens) it emits
    // displaySetupRequired instead, and QML asks before connecting.
    Q_INVOKABLE void connectToHost(const QString& hostId);
    // Per-workstation display setup kept in the Client's local settings.
    // Keys: configured, layoutChoice, virtualMode1, virtualMode2, scalingChoice,
    // canMatchClient, matchClientReason, clientResolution, virtualModes.
    Q_INVOKABLE QVariantMap displaySetup(const QString& hostId) const;
    Q_INVOKABLE bool saveDisplaySetup(const QString& hostId, int layoutChoice, const QString& virtualMode1,
                                      const QString& virtualMode2, int scalingChoice);
    Q_INVOKABLE void logout();
    // Hands the prepared brokered Session to QML (JavaScript ownership),
    // once, after connectReady().
    Q_INVOKABLE Session* takeSession();

    QString brokerAddress() const;
    bool configured() const;
    bool signedIn() const { return !m_Username.isEmpty() && m_Token->hasToken(); }
    bool busy() const { return !m_BusyText.isEmpty(); }
    QString busyText() const { return m_BusyText; }
    QString username() const { return m_Username; }
    QVariantList hosts() const { return m_Hosts; }

signals:
    void configurationChanged();
    void stateChanged();
    void hostsChanged();
    // Generic, user-facing message (never broker- or host-supplied text).
    void errorOccurred(QString message);
    void connectReady(QString hostName);
    void displaySetupRequired(QString hostId, QString hostName, QString reason);

private:
    // Shared with Session worker threads (re-admission) and keepalive tasks.
    class SharedToken
    {
    public:
        QString get() const { QMutexLocker lock(&m_Lock); return m_Token; }
        void set(QString token) { QMutexLocker lock(&m_Lock); m_Token = std::move(token); }
        void clear() { QMutexLocker lock(&m_Lock); m_Token.fill(QChar('\0')); m_Token.clear(); }
        bool hasToken() const { QMutexLocker lock(&m_Lock); return !m_Token.isEmpty(); }
    private:
        mutable QMutex m_Lock;
        QString m_Token;
    };

    struct HostDefaults {
        bool found = false;
        int videoProfile = 0;
        int captureSource = 0;
        QString scalingMode;
        QString hostLayout;
        QString virtualMode1;
        QString virtualMode2;
        QVector<int> profileBitratesKbps;
    };

    PlankBrokerClient::Config clientConfig() const;
    void setBusy(const QString& text);
    void handleBrokerError(const PlankBrokerError& error, bool connecting);
    void signOutLocally(const QString& message = QString());
    HostDefaults bookmarkDefaultsFor(const QString& hostName) const;
    QString hostNameFor(const QString& hostId) const;
    void startKeepalive(const QString& hostId, Session* session);
    void stopKeepalive();
    void scheduleKeepalive();
    void sendKeepalive();

    StreamingPreferences* m_Preferences;
    QPointer<ComputerManager> m_ComputerManager;
    std::shared_ptr<SharedToken> m_Token;
    QString m_Username;
    QString m_BusyText;
    QVariantList m_Hosts;
    QPointer<Session> m_PendingSession;
    // Brokered computers are never persisted; they must outlive their Session.
    QVector<NvComputer*> m_Computers;

    QTimer m_KeepaliveTimer;
    PlankBroker::KeepaliveSchedule m_KeepaliveSchedule;
    QString m_KeepaliveHostId;
    QPointer<Session> m_KeepaliveSession;
    bool m_KeepaliveInFlight = false;
    quint64 m_Generation = 0;
};

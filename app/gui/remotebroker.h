#pragma once

#include "backend/plankbroker.h"
#include "backend/plankbrokerclient.h"
#include "backend/plankpasskey.h"

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
// keepalive while a brokered stream runs. On macOS it also drives Touch ID
// sign-in through the bundled plank-passkey helper (section 13.4).
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
    // Touch ID sign-in: helper present (macOS), local keys for the
    // configured relying party ({username, credentialId, mapping, created}),
    // and the user names that have one.
    Q_PROPERTY(bool passkeySupported READ passkeySupported CONSTANT)
    Q_PROPERTY(QVariantList passkeys READ passkeys NOTIFY passkeysChanged)
    Q_PROPERTY(QStringList passkeyUsers READ passkeyUsers NOTIFY passkeysChanged)
    Q_PROPERTY(bool passkeyBusy READ passkeyBusy NOTIFY passkeysChanged)

public:
    explicit RemoteBroker(StreamingPreferences* preferences, QObject* parent = nullptr);
    ~RemoteBroker() override;

    Q_INVOKABLE void initialize(ComputerManager* computerManager);
    Q_INVOKABLE void signIn(const QString& username, QString password, QString otp);
    // Passkey sign-in; emits passkeyFallback() when password + code is needed.
    Q_INVOKABLE void signInWithPasskey(const QString& username);
    Q_INVOKABLE void refreshPasskeys();
    Q_INVOKABLE void createPasskey(const QString& username);
    Q_INVOKABLE void removePasskey(const QString& username);
    Q_INVOKABLE void refreshHosts();
    // Connects with the saved display setup; without one (or when the saved
    // "match my displays" no longer fits the current screens) it emits
    // displaySetupRequired instead, and QML asks before connecting.
    Q_INVOKABLE void connectToHost(const QString& hostId);
    // Per-workstation display setup kept in the Client's local settings.
    // Keys: configured, layoutChoice, virtualMode1, virtualMode2, scalingChoice,
    // canMatchClient, matchClientReason, matchClientSummary, matchClientFitted,
    // clientResolution, virtualModes.
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
    bool passkeySupported() const { return m_PasskeyHelper.available(); }
    QVariantList passkeys() const { return m_Passkeys; }
    QStringList passkeyUsers() const;
    bool passkeyBusy() const { return m_PasskeyBusy; }

signals:
    void configurationChanged();
    void stateChanged();
    void hostsChanged();
    // Generic, user-facing message (never broker- or host-supplied text).
    void errorOccurred(QString message);
    void connectReady(QString hostName);
    void displaySetupRequired(QString hostId, QString hostName, QString reason);
    void passkeysChanged();
    // No usable passkey for this sign-in: ask for password + code instead.
    void passkeyFallback(QString message);
    // Mapping line for the administrator ("passkey:<id>,<SPKI>"; not secret).
    void passkeyCreated(QString username, QString mapping);
    void passkeyError(QString message);

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
    QString passkeyRpId() const;
    void finishSignIn(QString token, const QString& confirmedUser);
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
    PlankPasskeyHelper m_PasskeyHelper;
    QVariantList m_Passkeys;
    bool m_PasskeyBusy = false;
    quint64 m_PasskeyGeneration = 0;
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

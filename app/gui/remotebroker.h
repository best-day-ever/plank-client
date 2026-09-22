#pragma once

#include "backend/plankbroker.h"
#include "backend/plankbrokerclient.h"
#include "backend/plankpasskey.h"
#include "backend/remotestreamsetup.h"

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
// broker session token (in memory; on macOS also remembered in the Keychain,
// never in the settings file), the host list, brokered
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
    // Connects with the display setup for the current screens (DisplayProfile:
    // this workstation's own, else the global one). Screens without one emit
    // displaySetupRequired with reason "new-screens" (unless the suggested
    // layout is accepted automatically), and QML runs the display setup
    // before connecting. A workstation on a fixed layout (older PLANK, remote
    // Mac) uses its saved setup and asks again when that no longer fits. When
    // the stream settings cannot work on the workstation it emits
    // streamSetupRequired instead of streaming with something else.
    Q_INVOKABLE void connectToHost(const QString& hostId);
    // Per-workstation display setup kept in the Client's local settings.
    // Keys: configured, layoutChoice, virtualMode1, virtualMode2, scalingChoice,
    // canMatchClient, matchClientReason, matchClientSummary, matchClientFitted,
    // clientResolution, virtualModes.
    Q_INVOKABLE QVariantMap displaySetup(const QString& hostId) const;
    Q_INVOKABLE bool saveDisplaySetup(const QString& hostId, int layoutChoice, const QString& virtualMode1,
                                      const QString& virtualMode2, int scalingChoice);
    // Per-workstation stream quality kept in the Client's local settings
    // (RemoteStreamSetup). Keys: useDefaults, source ("host", "bookmark",
    // "defaults", "builtin"), captureSource, videoProfile, officeBitratesKbps,
    // internetBitratesKbps, platform (0 = not known yet), encodingModes,
    // defaults (what "use the defaults" gives: captureSource, videoProfile,
    // officeBitratesKbps, internetBitratesKbps).
    Q_INVOKABLE QVariantMap streamSetup(const QString& hostId) const;
    Q_INVOKABLE bool saveStreamSetup(const QString& hostId, bool useDefaults, int captureSource,
                                     int videoProfile, const QVariantList& officeBitratesKbps,
                                     const QVariantList& internetBitratesKbps);
    // Why the workstation cannot use this choice, as far as its last connect
    // told us; empty when it can (or nothing is known yet).
    Q_INVOKABLE QString streamProfileProblem(const QString& hostId, int captureSource, int videoProfile) const;
    // Remote access defaults for workstations without their own settings.
    // Keys: custom, captureSource, videoProfile, officeBitratesKbps, internetBitratesKbps.
    Q_INVOKABLE QVariantMap remoteStreamDefaults() const;
    Q_INVOKABLE bool saveRemoteStreamDefaults(int captureSource, int videoProfile,
                                              const QVariantList& officeBitratesKbps,
                                              const QVariantList& internetBitratesKbps);
    Q_INVOKABLE void resetRemoteStreamDefaults();
    Q_INVOKABLE void logout();
    // Hands the prepared brokered Session to QML (JavaScript ownership),
    // once, after connectReady().
    Q_INVOKABLE Session* takeSession();
    // The workstation a brokered stream runs on now, else empty.
    Q_INVOKABLE QString streamingHostId() const { return m_KeepaliveHostId; }

    // For the first sign-in wizard (OnboardingController), which runs its own
    // broker conversation and ends in the same signed-in state.
    PlankBrokerClient::Config brokerClientConfig() const { return clientConfig(); }
    QString passkeyRelyingParty() const { return passkeyRpId(); }
    // The plank-passkey helper, or empty where there is none.
    QString passkeyHelperProgram() const
    {
        return m_PasskeyHelper.available() ? m_PasskeyHelper.program() : QString();
    }
    // Takes over a session the wizard obtained, exactly like a sign-in here
    // (Keychain, host list).
    void adoptSession(QString token, const QString& confirmedUser);

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
    // The saved (or default) stream settings do not fit the workstation;
    // reason is user-facing.
    void streamSetupRequired(QString hostId, QString hostName, QString reason);
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

    PlankBrokerClient::Config clientConfig() const;
    QString passkeyRpId() const;
    void finishSignIn(QString token, const QString& confirmedUser);
    void setBusy(const QString& text);
    void handleBrokerError(const PlankBrokerError& error, bool connecting);
    void signOutLocally(const QString& message = QString());
    // LAN bookmarks that could seed a workstation's first stream settings.
    QVector<RemoteStreamSetup::BookmarkCandidate> bookmarkCandidates() const;
    bool bookmarkSeedFor(const QString& hostId, const QString& hostName, RemoteStreamSetup::Setup& seed) const;
    RemoteStreamSetup::Resolution resolveStreamSetup(const QString& hostId, int platform) const;
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

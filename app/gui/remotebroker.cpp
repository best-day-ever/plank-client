#include "backend/betadiagnostics.h"
#include "remotebroker.h"

#include "backend/brokersessionstore.h"
#include "backend/brokerdevicekey.h"
#include "backend/clientdisplayprobe.h"
#include "backend/computermanager.h"
#include "backend/displayplanner.h"
#include "backend/displayprofile.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/onboardingstate.h"
#include "backend/plankenrollment.h"
#include "backend/outputtopology.h"
#include "backend/remotedisplaysetup.h"
#include "backend/remotestreamsetup.h"
#include "settings/streamingpreferences.h"
#include "streaming/session.h"

#include <QDebug>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QElapsedTimer>
#include <QUuid>
#include <QQmlEngine>
#include <QThreadPool>
#include <QThread>
#include <QCoreApplication>

#include <algorithm>
#include <memory>

namespace {

QElapsedTimer& monotonicClock()
{
    static QElapsedTimer clock = [] { QElapsedTimer timer; timer.start(); return timer; }();
    return clock;
}

qint64 nowMs() { return monotonicClock().elapsed(); }

// Maps broker failures onto the exception types the Session re-auth paths
// already classify (401 while authenticating is terminal, TLS is terminal,
// network and 429 are retryable).
[[noreturn]] void throwForSession(const PlankBrokerError& error)
{
    qWarning() << "Remote access broker admission failed; kind" << static_cast<int>(error.kind());
    switch (error.kind()) {
    case PlankBrokerError::Network:
    case PlankBrokerError::DeviceKeyUnavailable:
        throw QtNetworkReplyException(QNetworkReply::TimeoutError, error.userMessage());
    case PlankBrokerError::Tls:
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, error.userMessage());
    case PlankBrokerError::RateLimited:
        throw GfeHttpResponseException(429, error.userMessage());
    case PlankBrokerError::Unavailable:
        throw GfeHttpResponseException(409, error.userMessage());
    case PlankBrokerError::Protocol:
        throw GfeHttpResponseException(400, error.userMessage());
    case PlankBrokerError::NotConfigured:
    case PlankBrokerError::Denied:
    case PlankBrokerError::SessionExpired:
    default:
        throw GfeHttpResponseException(401, error.userMessage());
    }
}

struct AdmissionContext {
    PlankBrokerClient::Config config;
    std::function<QString()> currentToken;
    std::function<quint64()> beginLease;
    std::function<void(quint64, qint64, int, const QString&)> renewLease;
    QString hostId;
    NvComputer* computer = nullptr;
};

}

RemoteBroker::RemoteBroker(StreamingPreferences* preferences, QObject* parent)
    : QObject(parent),
      m_Preferences(preferences),
      m_Token(std::make_shared<SharedToken>())
{
    m_Diagnostics = new BetaDiagnostics(BetaDiagnostics::defaultDirectory(),this);
    m_DiagnosticSampleTimer.setInterval(5000);
    connect(&m_DiagnosticSampleTimer,&QTimer::timeout,this,[this] {
        if (m_KeepaliveSession) m_Diagnostics->sample(m_KeepaliveSession->diagnosticRenderedFps(),
                m_KeepaliveSession->diagnosticVideoMbps(),m_KeepaliveSession->currentNetworkRttMs(),
                m_KeepaliveSession->diagnosticReceivedAgeMs(),m_KeepaliveSession->diagnosticStatisticsAgeMs());
    });
    connect(m_Preferences,&StreamingPreferences::brokerChanged,this,&RemoteBroker::configureDiagnostics);
    monotonicClock();
    m_KeepaliveTimer.setSingleShot(true);
    connect(&m_KeepaliveTimer, &QTimer::timeout, this, &RemoteBroker::sendKeepalive);
    m_PresenceTimer.setInterval(15000);
    connect(&m_PresenceTimer, &QTimer::timeout, this, [this]() { sendPresence(true); });
    connect(m_Preferences, &StreamingPreferences::brokerChanged,
            this, &RemoteBroker::configurationChanged);
    connect(m_Preferences, &StreamingPreferences::brokerChanged,
            this, &RemoteBroker::stateChanged);
    // The relying party is a broker setting; its keys are listed per rp.
    connect(m_Preferences, &StreamingPreferences::brokerChanged,
            this, &RemoteBroker::refreshPasskeys);
}

RemoteBroker::~RemoteBroker()
{
    m_Token->clear();
    // Brokered computers may still be referenced by a Session being torn
    // down at exit; they are tiny, so they are intentionally not freed here.
}

void RemoteBroker::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    // Resume a remembered session (macOS Keychain or Windows DPAPI). If the broker has since
    // expired it, the first host list returns 401 and signOutLocally drops it.
    if (!signedIn() && configured()) {
        BrokerSessionStore::Saved saved;
        if (BrokerSessionStore::load(brokerAddress(), saved)) {
            BrokerSessionStore::rememberUsername(brokerAddress(), saved.username);
            if (BrokerSessionStore::needsDeviceSignIn(saved)) {
                saved.token.fill(QChar('\0'));
                BrokerSessionStore::clear(brokerAddress());
                reportSessionNotice(tr("Sign in once more with your password and authenticator code to keep this Windows device signed in. Your username has been remembered."));
                return;
            }
            m_Token->set(saved.token, saved.deviceBound);
            saved.token.fill(QChar('\0'));
            m_Username = saved.username;
            configureDiagnostics();
            emit stateChanged();
        }
    }
}

PlankBrokerClient::Config RemoteBroker::clientConfig() const
{
    const auto token = m_Token;
    return BrokerDeviceKey::clientConfig(m_Preferences->brokerHost,
                static_cast<quint16>(qBound(0, m_Preferences->brokerPort, 65535)),
                m_Preferences->brokerPins, [token]() { return token->proofRequired(); },
                m_PasskeyHelper.program());
}

QString RemoteBroker::rememberedUsername() const
{
    return BrokerSessionStore::rememberedUsername(brokerAddress());
}

void RemoteBroker::reportSessionNotice(const QString& text)
{
    if (m_SessionNotice == text) return;
    m_SessionNotice = text;
    emit stateChanged();
}

QString RemoteBroker::brokerAddress() const
{
    return QStringLiteral("%1:%2").arg(m_Preferences->brokerHost).arg(m_Preferences->brokerPort);
}

bool RemoteBroker::configured() const
{
    return !PlankBroker::normalizePins(m_Preferences->brokerPins).isEmpty() &&
            PlankBroker::isEndpointName(m_Preferences->brokerHost.trimmed()) &&
            m_Preferences->brokerPort >= 1 && m_Preferences->brokerPort <= 65535;
}

void RemoteBroker::setBusy(const QString& text)
{
    if (m_BusyText != text) {
        m_BusyText = text;
        emit stateChanged();
    }
}

void RemoteBroker::signOutLocally(const QString& message)
{
    ++m_Generation;
    m_TouchIdSetupPending = false;
    m_TouchIdSetupBusy = false;
    m_TouchIdSetupError.clear();
    stopKeepalive();
    m_Token->clear();
    BrokerSessionStore::clear(brokerAddress());
    m_Username.clear();
    configureDiagnostics();
    m_Hosts.clear();
    m_BusyText.clear();
    if (!message.isEmpty()) m_SessionNotice = message;
    emit hostsChanged();
    emit stateChanged();
    if (!message.isEmpty()) {
        emit errorOccurred(message);
    }
}

void RemoteBroker::handleBrokerError(const PlankBrokerError& error, bool connecting)
{
    setBusy(QString());
    if (error.kind() == PlankBrokerError::SessionExpired) {
        signOutLocally(error.userMessage());
        return;
    }
    if (error.kind() == PlankBrokerError::DeviceKeyUnavailable) {
        reportSessionNotice(error.userMessage());
        return;
    }
    if (connecting && error.kind() == PlankBrokerError::Denied) {
        emit errorOccurred(tr("The remote access server did not admit a connection to this workstation."));
        refreshHosts();
        return;
    }
    emit errorOccurred(error.userMessage());
}

void RemoteBroker::signIn(const QString& username, QString password, QString otp)
{
    const QString user = username.trimmed();
    if (busy()) {
        password.fill(QChar('\0'));
        return;
    }
    if (!configured()) {
        password.fill(QChar('\0'));
        emit errorOccurred(PlankBrokerError(PlankBrokerError::NotConfigured).userMessage());
        return;
    }
    if (user.isEmpty() || password.isEmpty() || !PlankBroker::isValidOtp(otp)) {
        password.fill(QChar('\0'));
        emit errorOccurred(tr("Enter your username, password and the 6-digit code from your authenticator app."));
        return;
    }

    setBusy(tr("Signing in..."));
    const PlankBrokerClient::Config config = clientConfig();
    const QString helperProgram = m_PasskeyHelper.program();
    const QString rpId = passkeyRpId();
    const quint64 generation = ++m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, helperProgram, rpId, generation, user, password, otp]() mutable {
        QString token;
        QString confirmedUser;
        bool offerTouchId = false;
        bool deviceBound = false;
        std::unique_ptr<PlankBrokerError> failure;
        try {
            PlankBrokerClient client(config);
            PlankBroker::AuthReply reply = client.start(user);
            // One challenge round is expected (password + code). A second
            // challenge cannot be answered with the same one-time code.
            QJsonArray responses;
            if (!PlankBroker::buildResponses(reply.prompts, user, password, otp, responses)) {
                throw PlankBrokerError(PlankBrokerError::Protocol);
            }
            reply = client.respond(reply.conversationId, responses);
            for (int i = 0; i < responses.size(); ++i) responses[i] = QString();
            if (reply.kind != PlankBroker::ReplyKind::Authenticated) {
                throw PlankBrokerError(PlankBrokerError::Denied);
            }
            token = reply.sessionToken;
            deviceBound = reply.deviceBound;
            confirmedUser = reply.username.isEmpty() ? user : reply.username;
            if (reply.passkeySetupAvailable) {
                const PlankPasskeyHelper helper(helperProgram);
                QVector<PlankPasskeyHelper::LocalKey> keys;
                if (helper.available() && helper.list(rpId, keys)) {
                    const QString normalized = PlankBroker::normalizePasskeyUsername(confirmedUser);
                    offerTouchId = std::none_of(keys.begin(), keys.end(), [&](const auto& key) {
                        return key.username == normalized;
                    });
                }
                if (!offerTouchId) {
                    try { client.skipPasskeySetup(token); } catch (const PlankBrokerError&) {
                        // The broker expires the short-lived password in five minutes.
                    }
                }
            }
            qInfo() << "Remote access session device-bound:" << reply.deviceBound;
        } catch (const PlankBrokerError& error) {
            failure = std::make_unique<PlankBrokerError>(error);
        }
        password.fill(QChar('\0'));
        otp.fill(QChar('\0'));
        QMetaObject::invokeMethod(qApp, [self, generation, token, confirmedUser, offerTouchId, deviceBound,
                                         failure = std::shared_ptr<PlankBrokerError>(failure.release())]() mutable {
            if (!self || generation != self->m_Generation) {
                token.fill(QChar('\0'));
                return;
            }
            if (failure) {
                self->handleBrokerError(*failure, false);
                return;
            }
            self->finishSignIn(token, confirmedUser, offerTouchId, deviceBound);
            token.fill(QChar('\0'));
        }, Qt::QueuedConnection);
    });
}

QString RemoteBroker::passkeyRpId() const
{
    const QString rpId = m_Preferences->passkeyRpId.trimmed().toLower();
    return PlankBroker::isPasskeyRpId(rpId) ? rpId : PlankBroker::defaultPasskeyRpId();
}

void RemoteBroker::finishSignIn(QString token, const QString& confirmedUser, bool offerTouchId, bool deviceBound)
{
    stopKeepalive();
    m_TouchIdSetupPending = offerTouchId;
    m_TouchIdSetupBusy = false;
    m_TouchIdSetupError.clear();
    m_Token->set(token, deviceBound);
    // Password + code and Touch ID both end here: remember the session.
    const bool saved = BrokerSessionStore::save(brokerAddress(), confirmedUser, token, deviceBound);
    BrokerSessionStore::rememberUsername(brokerAddress(), confirmedUser);
    m_SessionNotice = saved ? QString() : tr("You are signed in, but this computer could not save the sign-in. You may need to sign in again after restarting Fernweh.");
    token.fill(QChar('\0'));
    // Lasting proof this Mac was used before, beyond the Keychain session
    // that sign-out and an expired session clear: no wizard on next launch.
    {
        QSettings settings;
        OnboardingState::recordBrokerSignIn(settings);
    }
    m_Username = confirmedUser;
    configureDiagnostics();
    setBusy(QString());
    emit stateChanged();
    refreshHosts();
    if (offerTouchId) emit touchIdSetupRequested();
}

void RemoteBroker::adoptSession(QString token, const QString& confirmedUser, bool deviceBound)
{
    if (token.isEmpty() || confirmedUser.isEmpty()) {
        token.fill(QChar('\0'));
        return;
    }
    // Anything still in flight belongs to the previous state.
    ++m_Generation;
    finishSignIn(token, confirmedUser, false, deviceBound);
    token.fill(QChar('\0'));
}

void RemoteBroker::signInWithPasskey(const QString& username)
{
    const QString user = PlankBroker::normalizePasskeyUsername(username);
    if (busy()) return;
    if (!configured()) {
        emit errorOccurred(PlankBrokerError(PlankBrokerError::NotConfigured).userMessage());
        return;
    }
    if (!PlankBroker::isPasskeyUsername(user) || !m_PasskeyHelper.available()) {
        emit passkeyFallback(tr("Sign in with your password and authenticator code."));
        return;
    }

    setBusy(tr("Signing in..."));
    const PlankBrokerClient::Config config = clientConfig();
    const QString rpId = passkeyRpId();
    const QString program = m_PasskeyHelper.program();
    const quint64 generation = ++m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, rpId, program, generation, user]() {
        PlankBrokerClient::PasskeySignIn outcome;
        std::shared_ptr<PlankBrokerError> failure;
        try {
            const PlankPasskeyHelper helper(program);
            outcome = PlankBrokerClient(config).signInWithPasskey(user, rpId,
                    [&](const PlankBroker::PasskeyRequest& request, PlankBroker::PasskeyAssertion& assertion) {
                QMetaObject::invokeMethod(qApp, [self, generation]() {
                    if (self && generation == self->m_Generation) {
                        self->setBusy(tr("Confirm with Touch ID..."));
                    }
                }, Qt::QueuedConnection);
                return helper.assertion(request, user, assertion);
            });
        } catch (const PlankBrokerError& error) {
            failure = std::make_shared<PlankBrokerError>(error);
        }
        QString token = outcome.reply.sessionToken;
        outcome.reply.sessionToken.fill(QChar('\0'));
        if (outcome.result == PlankBrokerClient::PasskeySignIn::Authenticated) {
            qInfo() << "Remote access session device-bound:" << outcome.reply.deviceBound;
        }
        const QString confirmedUser = outcome.reply.username.isEmpty() ? user : outcome.reply.username;
        const bool deviceBound = outcome.reply.deviceBound;
        const auto result = outcome.result;
        QMetaObject::invokeMethod(qApp, [self, generation, token, confirmedUser, result, failure, deviceBound]() mutable {
            if (!self || generation != self->m_Generation) {
                token.fill(QChar('\0'));
                return;
            }
            if (failure) {
                self->handleBrokerError(*failure, false);
                return;
            }
            switch (result) {
            case PlankBrokerClient::PasskeySignIn::Authenticated:
                self->finishSignIn(token, confirmedUser, false, deviceBound);
                return;
            case PlankBrokerClient::PasskeySignIn::NotConfirmed:
                self->setBusy(QString());
                emit self->errorOccurred(tr("Touch ID was not confirmed. Try again, or sign in with your password and authenticator code."));
                return;
            case PlankBrokerClient::PasskeySignIn::Fallback:
            default:
                self->setBusy(QString());
                emit self->passkeyFallback(tr("Touch ID sign-in did not work for this account. Sign in with your password and authenticator code."));
                return;
            }
        }, Qt::QueuedConnection);
    });
}

QStringList RemoteBroker::passkeyUsers() const
{
    QStringList users;
    for (const QVariant& value : m_Passkeys) {
        const QString user = value.toMap().value(QStringLiteral("username")).toString();
        if (!users.contains(user)) users.append(user);
    }
    return users;
}

void RemoteBroker::refreshPasskeys()
{
    if (!m_PasskeyHelper.available()) return;
    const QString rpId = passkeyRpId();
    const QString program = m_PasskeyHelper.program();
    const quint64 generation = ++m_PasskeyGeneration;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, rpId, program, generation]() {
        QVector<PlankPasskeyHelper::LocalKey> keys;
        if (!PlankPasskeyHelper(program).list(rpId, keys)) {
            qWarning() << "plank-passkey could not list the local passkeys";
        }
        QMetaObject::invokeMethod(qApp, [self, generation, keys]() {
            if (!self || generation != self->m_PasskeyGeneration) return;
            QVariantList list;
            for (const PlankPasskeyHelper::LocalKey& key : keys) {
                QVariantMap entry;
                entry.insert(QStringLiteral("username"), key.username);
                entry.insert(QStringLiteral("credentialId"), key.credentialId);
                entry.insert(QStringLiteral("mapping"), key.mapping);
                entry.insert(QStringLiteral("created"), key.created);
                list.append(entry);
            }
            self->m_Passkeys = list;
            emit self->passkeysChanged();
        }, Qt::QueuedConnection);
    });
}

void RemoteBroker::createPasskey(const QString& username)
{
    const QString user = PlankBroker::normalizePasskeyUsername(username);
    if (m_PasskeyBusy || !m_PasskeyHelper.available()) return;
    if (!PlankBroker::isPasskeyUsername(user)) {
        emit passkeyError(tr("Enter your studio user name."));
        return;
    }
    m_PasskeyBusy = true;
    emit passkeysChanged();
    const QString rpId = passkeyRpId();
    const QString program = m_PasskeyHelper.program();
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, rpId, program, user]() {
        PlankPasskeyHelper::CreatedKey key;
        const bool created = PlankPasskeyHelper(program).create(rpId, user, key);
        QMetaObject::invokeMethod(qApp, [self, user, created, key]() {
            if (!self) return;
            self->m_PasskeyBusy = false;
            emit self->passkeysChanged();
            if (created) {
                emit self->passkeyCreated(user, key.mapping);
            } else {
                emit self->passkeyError(tr("Touch ID sign-in could not be set up on this Mac."));
            }
            self->refreshPasskeys();
        }, Qt::QueuedConnection);
    });
}

void RemoteBroker::setUpTouchIdAfterSignIn()
{
    if (!m_TouchIdSetupPending || m_TouchIdSetupBusy || !signedIn() || !m_PasskeyHelper.available()) return;
    m_TouchIdSetupBusy = true;
    m_TouchIdSetupError.clear();
    emit stateChanged();
    const QString program = m_PasskeyHelper.program();
    const QString rpId = passkeyRpId();
    const QString user = PlankBroker::normalizePasskeyUsername(m_Username);
    QString token = m_Token->get();
    const PlankBrokerClient::Config config = clientConfig();
    const quint64 generation = m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, program, rpId, user, token, config, generation]() mutable {
        const PlankPasskeyHelper helper(program);
        PlankPasskeyHelper::CreatedKey key;
        const bool created = helper.create(rpId, user, key) && PlankEnrollment::isPasskeyMapping(key.mapping);
        bool added = false;
        if (created) {
            for (int attempt = 0; attempt < 2 && !added; ++attempt) {
                try {
                    PlankBrokerClient(config).setupPasskey(token, key.mapping);
                    added = true;
                } catch (const PlankBrokerError& error) {
                    if (error.kind() != PlankBrokerError::Network || attempt == 1) break;
                }
            }
            if (!added) helper.remove(rpId, user);
        }
        token.fill(QChar('\0'));
        QMetaObject::invokeMethod(qApp, [self, generation, created, added]() {
            if (!self || generation != self->m_Generation) return;
            self->m_TouchIdSetupBusy = false;
            if (added) {
                self->m_TouchIdSetupPending = false;
                self->m_TouchIdSetupError.clear();
                self->refreshPasskeys();
                emit self->touchIdSetupCompleted();
            } else {
                self->m_TouchIdSetupError = created ?
                            tr("Touch ID couldn't be added to your account. Try again or choose Not now.") :
                            tr("Touch ID wasn't set up on this Mac. Try again or choose Not now.");
            }
            emit self->stateChanged();
        }, Qt::QueuedConnection);
    });
}

void RemoteBroker::skipTouchIdAfterSignIn()
{
    if (!m_TouchIdSetupPending || m_TouchIdSetupBusy) return;
    m_TouchIdSetupPending = false;
    m_TouchIdSetupError.clear();
    emit stateChanged();
    QString token = m_Token->get();
    const PlankBrokerClient::Config config = clientConfig();
    QThreadPool::globalInstance()->start([token, config]() mutable {
        try { PlankBrokerClient(config).skipPasskeySetup(token); } catch (const PlankBrokerError&) {
            // The broker still expires the password after five minutes.
        }
        token.fill(QChar('\0'));
    });
}

void RemoteBroker::removePasskey(const QString& username)
{
    const QString user = PlankBroker::normalizePasskeyUsername(username);
    if (m_PasskeyBusy || !m_PasskeyHelper.available() || !PlankBroker::isPasskeyUsername(user)) return;
    m_PasskeyBusy = true;
    emit passkeysChanged();
    const QString rpId = passkeyRpId();
    const QString program = m_PasskeyHelper.program();
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, rpId, program, user]() {
        const PlankPasskeyHelper::Result result = PlankPasskeyHelper(program).remove(rpId, user);
        // Exit 3: nothing left to remove, which is the requested state.
        const bool removed = result.ran &&
                (result.exitCode == PlankPasskeyHelper::Ok || result.exitCode == PlankPasskeyHelper::NoMatchingKey);
        QMetaObject::invokeMethod(qApp, [self, removed]() {
            if (!self) return;
            self->m_PasskeyBusy = false;
            emit self->passkeysChanged();
            if (!removed) {
                emit self->passkeyError(tr("Touch ID sign-in could not be removed."));
            }
            self->refreshPasskeys();
        }, Qt::QueuedConnection);
    });
}

void RemoteBroker::refreshHosts()
{
    refreshHostsImpl(true);
}

void RemoteBroker::refreshHostsImpl(bool showBusy)
{
    if (!signedIn() || busy()) return;
    const auto refresh = m_Token->beginRefresh();
    if (!refresh) return;
    if (showBusy) setBusy(tr("Loading workstations..."));
    const PlankBrokerClient::Config config = clientConfig();
    const auto token = m_Token;
    const quint64 generation = m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, generation, refresh = *refresh]() {
        QVector<PlankBroker::Host> hosts;
        std::shared_ptr<PlankBrokerError> failure;
        try {
            hosts = PlankBrokerClient(config).hosts(token->getForRevision(refresh.authentication));
        } catch (const PlankBrokerError& error) {
            failure = std::make_shared<PlankBrokerError>(error);
        }
        QMetaObject::invokeMethod(qApp, [self, token, generation, refresh, hosts, failure]() {
            if (!self) return;
            if (!token->finishRefresh(refresh)) return;
            if (generation != self->m_Generation) return;
            if (failure) {
                self->handleBrokerError(*failure, false);
                return;
            }
            QVariantList list;
            for (const PlankBroker::Host& host : hosts) {
                QVariantMap entry;
                entry.insert(QStringLiteral("id"), host.id);
                entry.insert(QStringLiteral("name"), host.name);
                entry.insert(QStringLiteral("online"), host.online);
                entry.insert(QStringLiteral("busy"), host.busy);
                entry.insert(QStringLiteral("connectedUsers"), host.connectedUsers);
                entry.insert(QStringLiteral("inUseBy"), host.inUseBy);
                entry.insert(QStringLiteral("connectable"), host.connectable);
                entry.insert(QStringLiteral("reason"), host.reason);
                list.append(entry);
            }
            self->m_Hosts = list;
            self->setBusy(QString());
            emit self->hostsChanged();
        }, Qt::QueuedConnection);
    });
}

QVector<RemoteStreamSetup::BookmarkCandidate> RemoteBroker::bookmarkCandidates() const
{
    QVector<RemoteStreamSetup::BookmarkCandidate> candidates;
    if (m_ComputerManager.isNull()) return candidates;
    const QVector<NvComputer*> computers = m_ComputerManager->getComputers();
    for (NvComputer* computer : computers) {
        QReadLocker lock(&computer->lock);
        RemoteStreamSetup::BookmarkCandidate candidate;
        candidate.name = computer->name;
        candidate.address = computer->manualAddress.isNull() ? QString() : computer->manualAddress.address();
        candidate.captureSource = computer->plankCaptureSource;
        candidate.videoProfile = computer->plankVideoProfile;
        candidate.bitratesKbps = computer->plankProfileBitratesKbps;
        candidates.append(candidate);
    }
    return candidates;
}

bool RemoteBroker::bookmarkSeedFor(const QString& hostId, const QString& hostName,
                                   RemoteStreamSetup::Setup& seed) const
{
    return RemoteStreamSetup::seedFromBookmarks(bookmarkCandidates(), hostId, hostName, seed);
}

RemoteStreamSetup::Resolution RemoteBroker::resolveStreamSetup(const QString& hostId, int platform) const
{
    QSettings settings;
    const RemoteStreamSetup::Setup host = RemoteStreamSetup::loadHost(settings, hostId);
    const RemoteStreamSetup::Setup defaults = RemoteStreamSetup::loadDefaults(settings);
    RemoteStreamSetup::Setup seed;
    const bool seeded = host.mode == RemoteStreamSetup::Unset &&
            bookmarkSeedFor(hostId, hostNameFor(hostId), seed);
    return RemoteStreamSetup::resolve(host, seeded ? &seed : nullptr, defaults, platform);
}

namespace {

// Stream settings the worker resolves once it knows the workstation's platform.
struct StreamInputs {
    RemoteStreamSetup::Setup host;
    bool seeded = false;
    RemoteStreamSetup::Setup seed;
    RemoteStreamSetup::Setup defaults;
};

// The stream settings do not fit the workstation: ask instead of launching.
struct StreamSettingsRequired {
    QString reason;
};

// Worker thread: section 10.2 steps 1-2 plus the ordinary pre-launch
// preparation (topology, app list). The returned computer is authorized with
// a one-use host token and pinned to the broker-supplied leaf. capabilities
// receives what the workstation reported, even when it throws.
NvComputer* prepareBrokeredComputer(const PlankBroker::Lease& lease, const QString& hostId,
                                    const QString& hostName, const StreamInputs& stream,
                                    const RemoteDisplaySetup::Setup& display,
                                    RemoteStreamSetup::Capabilities& capabilities,
                                    bool& authenticatedGreeter)
{
    const NvAddress address(lease.endpoint, lease.port);
    NvHTTP http(address);
    http.setPinnedCertificateSha256(lease.hostCertSha256);
    const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR);
    NvComputer probed(http, serverInfo);
    if (!probed.plankAuthentication) {
        throw GfeHttpResponseException(400, "The remote workstation does not offer BDE fernweh authentication");
    }
    const bool macHost = probed.plankFeatureFlags == NvOutputTopology::FixedCaptureFlags;
    capabilities.known = true;
    capabilities.platform = macHost ? RemoteStreamSetup::MacPlatform : RemoteStreamSetup::LinuxPlatform;
    capabilities.featureFlags = probed.plankFeatureFlags;
    capabilities.encodingModes = probed.plankEncodingModes;

    const RemoteStreamSetup::Resolution resolution = RemoteStreamSetup::resolve(
                stream.host, stream.seeded ? &stream.seed : nullptr, stream.defaults, capabilities.platform);
    const QString problem = RemoteStreamSetup::problemFor(resolution.setup, capabilities);
    if (!problem.isEmpty()) {
        qWarning() << "Remote stream settings do not fit" << hostId << ": profile"
                   << resolution.setup.videoProfile << "capture" << resolution.setup.captureSource
                   << "source" << resolution.source;
        throw StreamSettingsRequired { problem };
    }
    const RemoteStreamSetup::Route route = lease.route == PlankBroker::Route::Direct ?
                RemoteStreamSetup::OfficeNetwork : RemoteStreamSetup::Internet;
    const QVector<int>& bitrates = RemoteStreamSetup::bitratesFor(resolution.setup, route);
    qInfo() << "Remote stream settings for" << hostId << ": profile" << resolution.setup.videoProfile
            << "capture" << resolution.setup.captureSource << "source" << resolution.source
            << "startup target" << RemoteStreamSetup::bitrateFor(resolution.setup, route) << "kbps";

    std::unique_ptr<NvComputer> computer(new NvComputer(
            address, hostName, resolution.setup.videoProfile, resolution.setup.captureSource, bitrates));
    {
        QWriteLocker lock(&computer->lock);
        // Connecting always has a display setup chosen for this workstation.
        if (display.configured) {
            computer->plankScalingMode = display.scalingMode;
            computer->plankHostLayout = display.hostLayout;
            computer->plankVirtualMode1 = display.virtualMode1;
            computer->plankVirtualMode2 = display.virtualMode2;
        }
        if (macHost) {
            // Match Client needs a GUI-thread display snapshot; remote Mac
            // hosts use virtual display 1 as their fixed desktop instead.
            computer->plankHostLayout = QStringLiteral("fixed");
        }
    }
    computer->update(probed);
    {
        QWriteLocker lock(&computer->lock);
        computer->activeAddress = address;
        computer->brokerHostCertSha256 = lease.hostCertSha256;
        computer->brokerHostId = hostId;
    }

    const QString token = http.authenticateGssapi(lease.username, lease.gssapiToken,
                                                  &authenticatedGreeter);
    NvOutputTopology topology;
    bool topologySupported;
    QString desktopMode;
    QString appleEncodingMode;
    {
        QReadLocker lock(&computer->lock);
        topologySupported = NvOutputTopology::supportsDescription(
                    computer->plankTopologyVersion, computer->plankFeatureFlags);
        desktopMode = computer->plankVirtualMode1;
        appleEncodingMode = StreamingPreferences::plankAppleEncodingMode(computer->plankVideoProfile);
    }
    if (topologySupported) {
        if (macHost) {
            topology = http.prepareMacDisplay(desktopMode, appleEncodingMode, 1);
        } else {
            QElapsedTimer readiness;
            readiness.start();
            for (;;) {
                try {
                    topology = http.getOutputTopology();
                    break;
                } catch (const GfeHttpResponseException& error) {
                    if (error.getStatusCode() != 425 || readiness.elapsed() >= 10000) throw;
                    QThread::msleep(300);
                }
            }
        }
        if (!macHost && topology.displayArrangementPublished()) {
            // Cached so the display setup can preview this workstation.
            capabilities.displayCapabilities = QString::fromUtf8(
                        QJsonDocument(topology.displayCapabilities.toJson()).toJson(QJsonDocument::Compact));
        }
    }
    const QVector<NvApp> apps = http.getAppList();
    {
        QWriteLocker lock(&computer->lock);
        computer->sessionToken = token;
        computer->sessionIdentityKey = http.hostIdentityKey();
        computer->authorizationState = NvComputer::AS_AUTHORIZED;
        if (topologySupported) {
            computer->outputTopology = topology;
        }
        computer->appList = apps;
    }
    return computer.release();
}

}

namespace {

// The virtual modes a Linux workstation accepted on its last connect (its
// cached feature flags); every qualified mode while nothing is known yet or
// for a Mac workstation, which sizes its desktop from mode 1 freely.
QStringList hostVirtualModes(QSettings& settings, const QString& hostId)
{
    const RemoteStreamSetup::Capabilities caps = RemoteStreamSetup::loadCapabilities(settings, hostId);
    return RemoteDisplaySetup::candidateModes(caps.known && caps.platform == RemoteStreamSetup::LinuxPlatform,
                                              caps.featureFlags);
}

}

QString RemoteBroker::hostNameFor(const QString& hostId) const
{
    for (const QVariant& value : std::as_const(m_Hosts)) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("id")).toString() == hostId) {
            return entry.value(QStringLiteral("name")).toString();
        }
    }
    return hostId;
}

QVariantMap RemoteBroker::displaySetup(const QString& hostId) const
{
    QVariantMap result;
    if (!PlankBroker::isHostId(hostId)) return result;
    QSettings settings;
    RemoteDisplaySetup::Setup setup = RemoteDisplaySetup::load(settings, hostId);
    // Only the modes this workstation accepted on its last connect.
    const QStringList modes = hostVirtualModes(settings, hostId);
    // The same probe the streaming Session uses, so the dialog shows and
    // proposes exactly what a connect will do.
    const QVector<NvClientDisplay> displays = ClientDisplayProbe::probe();
    const ClientDisplayProbe::MatchPreview match = ClientDisplayProbe::matchPreview(displays, modes);
    const bool canMatch = match.ok;
    const bool configured = setup.configured;
    if (!configured) {
        setup = RemoteDisplaySetup::proposal(displays, modes);
    }
    result.insert(QStringLiteral("configured"), configured);
    result.insert(QStringLiteral("layoutChoice"), RemoteDisplaySetup::choiceForLayout(setup.hostLayout));
    result.insert(QStringLiteral("virtualMode1"), setup.virtualMode1);
    result.insert(QStringLiteral("virtualMode2"), setup.virtualMode2);
    result.insert(QStringLiteral("scalingChoice"), RemoteDisplaySetup::choiceForScaling(setup.scalingMode));
    result.insert(QStringLiteral("canMatchClient"), canMatch);
    result.insert(QStringLiteral("matchClientReason"), match.reason);
    result.insert(QStringLiteral("matchClientSummary"), ClientDisplayProbe::matchSummary(match));
    result.insert(QStringLiteral("matchClientFitted"), match.fitted);
    result.insert(QStringLiteral("clientResolution"), ClientDisplayProbe::describe(displays));
    result.insert(QStringLiteral("virtualModes"), modes);
    return result;
}

bool RemoteBroker::saveDisplaySetup(const QString& hostId, int layoutChoice, const QString& virtualMode1,
                                    const QString& virtualMode2, int scalingChoice)
{
    if (!PlankBroker::isHostId(hostId)) return false;
    RemoteDisplaySetup::Setup setup;
    setup.hostLayout = RemoteDisplaySetup::layoutForChoice(layoutChoice);
    setup.virtualMode1 = virtualMode1;
    setup.virtualMode2 = virtualMode2;
    setup.scalingMode = RemoteDisplaySetup::scalingForChoice(scalingChoice);
    QSettings settings;
    const bool saved = RemoteDisplaySetup::save(settings, hostId, setup);
    if (saved) settings.sync();
    return saved;
}

namespace {

QVariantList bitrateList(const QVector<int>& bitrates)
{
    return StreamingPreferences::plankProfileBitratesToVariantList(bitrates);
}

QVariantMap streamMap(const RemoteStreamSetup::Setup& setup)
{
    QVariantMap map;
    map.insert(QStringLiteral("captureSource"), setup.captureSource);
    map.insert(QStringLiteral("videoProfile"), setup.videoProfile);
    map.insert(QStringLiteral("officeBitratesKbps"), bitrateList(setup.officeBitratesKbps));
    map.insert(QStringLiteral("internetBitratesKbps"), bitrateList(setup.internetBitratesKbps));
    return map;
}

bool streamFromQml(int captureSource, int videoProfile, const QVariantList& office, const QVariantList& internet,
                   RemoteStreamSetup::Setup& setup)
{
    setup.mode = RemoteStreamSetup::Custom;
    setup.captureSource = captureSource;
    setup.videoProfile = videoProfile;
    return StreamingPreferences::plankProfileBitratesFromVariantList(office, setup.officeBitratesKbps) &&
           StreamingPreferences::plankProfileBitratesFromVariantList(internet, setup.internetBitratesKbps) &&
           RemoteStreamSetup::isValid(setup);
}

QString sourceName(RemoteStreamSetup::Source source)
{
    switch (source) {
    case RemoteStreamSetup::FromHost: return QStringLiteral("host");
    case RemoteStreamSetup::FromBookmark: return QStringLiteral("bookmark");
    case RemoteStreamSetup::FromDefaults: return QStringLiteral("defaults");
    case RemoteStreamSetup::FromBuiltIn: default: return QStringLiteral("builtin");
    }
}

}

QVariantMap RemoteBroker::streamSetup(const QString& hostId) const
{
    QVariantMap result;
    if (!PlankBroker::isHostId(hostId)) return result;
    QSettings settings;
    const RemoteStreamSetup::Capabilities caps = RemoteStreamSetup::loadCapabilities(settings, hostId);
    const RemoteStreamSetup::Setup host = RemoteStreamSetup::loadHost(settings, hostId);
    const RemoteStreamSetup::Setup defaults = RemoteStreamSetup::loadDefaults(settings);
    const RemoteStreamSetup::Resolution effective = resolveStreamSetup(hostId, caps.platform);
    // "Use the defaults" skips this workstation's own choice and any bookmark.
    RemoteStreamSetup::Setup follow;
    follow.mode = RemoteStreamSetup::FollowDefaults;
    const RemoteStreamSetup::Resolution fromDefaults = RemoteStreamSetup::resolve(follow, nullptr, defaults, caps.platform);

    result = streamMap(effective.setup);
    result.insert(QStringLiteral("useDefaults"), effective.source == RemoteStreamSetup::FromDefaults ||
                                                 effective.source == RemoteStreamSetup::FromBuiltIn);
    result.insert(QStringLiteral("saved"), host.mode != RemoteStreamSetup::Unset);
    result.insert(QStringLiteral("source"), sourceName(effective.source));
    result.insert(QStringLiteral("platform"), caps.platform);
    result.insert(QStringLiteral("encodingModes"), caps.encodingModes);
    result.insert(QStringLiteral("defaults"), streamMap(fromDefaults.setup));
    return result;
}

bool RemoteBroker::saveStreamSetup(const QString& hostId, bool useDefaults, int captureSource, int videoProfile,
                                   const QVariantList& officeBitratesKbps, const QVariantList& internetBitratesKbps)
{
    if (!PlankBroker::isHostId(hostId)) return false;
    RemoteStreamSetup::Setup setup;
    if (useDefaults) {
        setup.mode = RemoteStreamSetup::FollowDefaults;
    } else if (!streamFromQml(captureSource, videoProfile, officeBitratesKbps, internetBitratesKbps, setup)) {
        return false;
    }
    QSettings settings;
    const bool saved = RemoteStreamSetup::saveHost(settings, hostId, setup);
    if (saved) settings.sync();
    return saved;
}

QString RemoteBroker::streamProfileProblem(const QString& hostId, int captureSource, int videoProfile) const
{
    QSettings settings;
    const RemoteStreamSetup::Capabilities caps = PlankBroker::isHostId(hostId) ?
                RemoteStreamSetup::loadCapabilities(settings, hostId) : RemoteStreamSetup::Capabilities();
    return RemoteStreamSetup::problemFor(captureSource, videoProfile, caps);
}

QVariantMap RemoteBroker::remoteStreamDefaults() const
{
    QSettings settings;
    const RemoteStreamSetup::Setup defaults = RemoteStreamSetup::loadDefaults(settings);
    const bool custom = defaults.mode == RemoteStreamSetup::Custom;
    QVariantMap result = streamMap(custom ? defaults : RemoteStreamSetup::builtInDefaults(RemoteStreamSetup::LinuxPlatform));
    result.insert(QStringLiteral("custom"), custom);
    return result;
}

bool RemoteBroker::saveRemoteStreamDefaults(int captureSource, int videoProfile,
                                            const QVariantList& officeBitratesKbps,
                                            const QVariantList& internetBitratesKbps)
{
    RemoteStreamSetup::Setup setup;
    if (!streamFromQml(captureSource, videoProfile, officeBitratesKbps, internetBitratesKbps, setup) ||
            captureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // The remote access defaults are for Linux workstations; remote Macs
        // keep their built-in default unless chosen per workstation.
        return false;
    }
    QSettings settings;
    const bool saved = RemoteStreamSetup::saveDefaults(settings, setup);
    if (saved) settings.sync();
    return saved;
}

void RemoteBroker::resetRemoteStreamDefaults()
{
    QSettings settings;
    RemoteStreamSetup::clearDefaults(settings);
    settings.sync();
}

void RemoteBroker::connectToHost(const QString& hostId)
{
    if (!signedIn() || busy() || !PlankBroker::isHostId(hostId)) return;
    const QString hostName = hostNameFor(hostId);
    QSettings settings;
    const QStringList modes = hostVirtualModes(settings, hostId);
    const QVector<NvClientDisplay> displays = ClientDisplayProbe::probe();
    const QString fingerprint = ClientDisplayProbe::fingerprint(displays);
    const RemoteStreamSetup::Capabilities cached = RemoteStreamSetup::loadCapabilities(settings, hostId);
    const bool macHost = cached.known && cached.platform == RemoteStreamSetup::MacPlatform;
    const DisplayProfile::Resolved profile = DisplayProfile::resolveForHost(settings, hostId, fingerprint);
    RemoteDisplaySetup::Setup display;
    if (macHost || profile.source == DisplayProfile::Resolved::Legacy) {
        // A fixed layout (older workstations, remote Macs): the settings
        // saved for this workstation, as before display profiles.
        display = RemoteDisplaySetup::load(settings, hostId);
        if (!display.configured) {
            emit displaySetupRequired(hostId, hostName, QString());
            return;
        }
        QString matchReason;
        if (!RemoteDisplaySetup::refreshMatchedModes(settings, hostId, display, displays, &matchReason, modes)) {
            // Screens changed since the setup was saved (e.g. a third monitor).
            emit displaySetupRequired(hostId, hostName, matchReason);
            return;
        }
        const QString unsupported = RemoteDisplaySetup::unsupportedModeReason(display, modes);
        if (!unsupported.isEmpty()) {
            // A saved virtual mode this workstation refused on its last connect.
            emit displaySetupRequired(hostId, hostName, unsupported);
            return;
        }
    } else {
        if (profile.source == DisplayProfile::Resolved::None) {
            // Screens this computer has no display setup for yet.
            if (!DisplayProfile::autoAccept(settings)) {
                emit displaySetupRequired(hostId, hostName, QStringLiteral("new-screens"));
                return;
            }
            DisplayProfile::saveGlobal(settings, fingerprint, ClientDisplayProbe::label(displays),
                                       DisplayPlanner::proposal(displays));
            settings.sync();
        }
        // Match client: the Session plans it from the display profile. A
        // Mac workstation seen for the first time takes mode 1 as its desktop.
        display = RemoteDisplaySetup::proposal(displays, modes);
        display.configured = true;
        display.hostLayout = QString::fromLatin1(NvOutputTopology::MatchClientHostLayout);
        display.scalingMode = QString::fromLatin1(NvOutputTopology::ScaledSpanMode);
    }
    StreamInputs stream;
    stream.host = RemoteStreamSetup::loadHost(settings, hostId);
    stream.defaults = RemoteStreamSetup::loadDefaults(settings);
    stream.seeded = stream.host.mode == RemoteStreamSetup::Unset &&
            bookmarkSeedFor(hostId, hostName, stream.seed);
    // Capabilities cached by an earlier connect are advisory only (the
    // settings dialog flags choices with them): the workstation may have been
    // upgraded or its encoder probe may have recovered since, and refusing
    // here would keep the cache from ever being refreshed. The check after
    // the probe below is authoritative and refreshes the cache.

    setBusy(tr("Connecting to %1...").arg(hostName));
    const PlankBrokerClient::Config config = clientConfig();
    const auto token = m_Token;
    const quint64 generation = m_Generation;
    stopKeepalive();
    const quint64 authentication = token->revision();
    const quint64 grant = token->beginLease(authentication);
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, generation, authentication, grant,
                                         hostId, hostName, stream, display]() {
        NvComputer* computer = nullptr;
        std::shared_ptr<PlankBrokerError> brokerFailure;
        QString hostFailure;
        QString streamProblem;
        RemoteStreamSetup::Capabilities capabilities;
        bool authenticatedGreeter = false;
        try {
            const QString sessionToken = token->getForRevision(authentication);
            if (!token->currentLease(authentication, grant)) return;
            // Time the grant conservatively from the request, not after host
            // probing (which can exceed the broker's initial 30-second TTL).
            const qint64 grantedAtMs = nowMs();
            auto connection = PlankBrokerClient(config).connectAndRenew(sessionToken, hostId);
            PlankBroker::Lease& lease = connection.lease;
            QMetaObject::invokeMethod(qApp, [self, generation, authentication, grant, hostId,
                                             sessionToken, grantedAtMs, expiresIn = connection.expiresIn,
                                             renewalFailure = connection.renewalFailure]() {
                if (self && generation == self->m_Generation && self->m_Token->currentLease(authentication, grant)) {
                    if (renewalFailure == PlankBrokerError::DeviceKeyUnavailable)
                        self->reportSessionNotice(PlankBrokerError(PlankBrokerError::DeviceKeyUnavailable).userMessage());
                    self->startKeepalive(hostId, authentication, grant, sessionToken, grantedAtMs, expiresIn);
                }
            }, Qt::QueuedConnection);
            // Office LAN: the broker hands out the workstation itself; the flow is identical.
            qInfo() << "Remote access route to" << hostId << ":"
                    << (lease.route == PlankBroker::Route::Direct ? "direct" : "relay");
            try {
                computer = prepareBrokeredComputer(lease, hostId, hostName, stream, display,
                                                   capabilities, authenticatedGreeter);
            } catch (const StreamSettingsRequired& required) {
                streamProblem = required.reason;
            }
            lease.gssapiToken.fill(QChar('\0'));
        } catch (const PlankBrokerError& error) {
            brokerFailure = std::make_shared<PlankBrokerError>(error);
        } catch (const GfeHttpResponseException& error) {
            qWarning() << "Brokered workstation preparation failed:" << error.toQString();
            hostFailure = error.getStatusCode() == 401 ?
                        tr("The workstation did not accept the remote sign-in. Connect again from the list.") :
                        tr("The workstation could not be prepared for streaming: %1").arg(error.toQString());
        } catch (const QtNetworkReplyException& error) {
            qWarning() << "Brokered workstation connection failed:" << error.toQString();
            hostFailure = error.getError() == QNetworkReply::SslHandshakeFailedError ?
                        tr("The workstation's identity did not match the remote access server's record.") :
                        tr("The workstation could not be reached through the remote access server.");
        }
        QMetaObject::invokeMethod(qApp, [self, generation, authentication, grant, hostId, hostName, computer,
                                         authenticatedGreeter,
                                         brokerFailure, hostFailure, streamProblem, capabilities]() {
            if (capabilities.known) {
                // Kept for the settings dialog and the next connect's check.
                QSettings settings;
                RemoteStreamSetup::saveCapabilities(settings, hostId, capabilities);
            }
            if (!self || generation != self->m_Generation || !self->m_Token->currentLease(authentication, grant)) {
                // Signed out meanwhile; the one-use host token is abandoned.
                delete computer;
                return;
            }
            if (brokerFailure) {
                self->stopKeepalive();
                self->handleBrokerError(*brokerFailure, true);
                return;
            }
            if (!streamProblem.isEmpty()) {
                self->stopKeepalive();
                // The lease is abandoned; a retry is a fresh broker connect.
                self->setBusy(QString());
                emit self->streamSetupRequired(hostId, hostName, streamProblem);
                return;
            }
            if (computer == nullptr) {
                self->stopKeepalive();
                self->setBusy(QString());
                emit self->errorOccurred(hostFailure);
                // Host tokens are one-use: any retry is a fresh broker connect.
                self->refreshHosts();
                return;
            }

            Session* session = nullptr;
            {
                QReadLocker lock(&computer->lock);
                for (NvApp& app : computer->appList) {
                    if (app.name == QStringLiteral("Desktop")) {
                        session = new Session(computer, app, nullptr, nullptr);
                        break;
                    }
                }
            }
            self->m_Computers.append(computer);
            if (session == nullptr) {
                self->stopKeepalive();
                self->setBusy(QString());
                emit self->errorOccurred(tr("The workstation did not provide its Desktop session."));
                return;
            }

            auto context = std::make_shared<AdmissionContext>();
            context->config = self->clientConfig();
            const auto sharedToken = self->m_Token;
            context->currentToken = [sharedToken, authentication]() { return sharedToken->getForRevision(authentication); };
            context->beginLease = [sharedToken, authentication]() { return sharedToken->beginLease(authentication); };
            const QPointer<Session> sessionOwner(session);
            context->renewLease = [self, sessionOwner, generation, authentication, hostId]
                    (quint64 grant, qint64 grantedAtMs, int expiresIn, const QString& sessionToken) {
                QMetaObject::invokeMethod(qApp, [self, sessionOwner, generation, authentication, hostId,
                                                 grant, grantedAtMs, expiresIn, sessionToken]() {
                    if (self && generation == self->m_Generation && sessionOwner &&
                            self->m_KeepaliveOwner == sessionOwner.data())
                        self->startKeepalive(hostId, authentication, grant, sessionToken, grantedAtMs, expiresIn);
                }, Qt::QueuedConnection);
            };
            context->hostId = hostId;
            context->computer = computer;
            session->setPlankBrokerAdmission([context](QString& username, QString& gssapiToken) {
                try {
                    const quint64 grant = context->beginLease();
                    const QString sessionToken = context->currentToken();
                    if (grant == 0 || sessionToken.isEmpty()) throw PlankBrokerError(PlankBrokerError::SessionExpired);
                    const qint64 grantedAtMs = nowMs();
                    auto connection = PlankBrokerClient(context->config).connectAndRenew(sessionToken, context->hostId);
                    PlankBroker::Lease& lease = connection.lease;
                    context->renewLease(grant, grantedAtMs, connection.expiresIn, sessionToken);
                    {
                        QWriteLocker lock(&context->computer->lock);
                        const NvAddress leased(lease.endpoint, lease.port);
                        context->computer->activeAddress = leased;
                        context->computer->manualAddress = leased;
                        context->computer->brokerHostCertSha256 = lease.hostCertSha256;
                    }
                    username = lease.username;
                    gssapiToken = lease.gssapiToken;
                    lease.gssapiToken.fill(QChar('\0'));
                } catch (const PlankBrokerError& error) {
                    throwForSession(error);
                }
            });
            session->setAuthenticatedGreeter(authenticatedGreeter);

            self->m_PendingSession = session;
            self->attachKeepaliveSession(session);
            self->setBusy(QString());
            emit self->connectReady(hostName);
        }, Qt::QueuedConnection);
    });
}

Session* RemoteBroker::takeSession()
{
    Session* session = m_PendingSession.data();
    m_PendingSession.clear();
    if (session != nullptr) {
        QQmlEngine::setObjectOwnership(session, QQmlEngine::JavaScriptOwnership);
    }
    return session;
}

void RemoteBroker::logout()
{
    QString token = m_Token->get();
    // Local sign-out clears shared auth state before the worker runs. Retain
    // this token's proof requirement so a missing key cannot downgrade logout.
    const PlankBrokerClient::Config config = clientConfig().withFrozenDeviceProofRequirement();
    signOutLocally();
    if (token.isEmpty()) return;
    QThreadPool::globalInstance()->start([config, token]() mutable {
        try {
            PlankBrokerClient(config).logout(token);
        } catch (const PlankBrokerError& error) {
            qInfo() << "Remote access logout was not confirmed:" << static_cast<int>(error.kind());
        }
        token.fill(QChar('\0'));
    });
}

void RemoteBroker::startKeepalive(const QString& hostId, quint64 authentication, quint64 grant,
                                 const QString& token, qint64 grantedAtMs, int expiresIn)
{
    if (!m_Token->currentLease(authentication, grant) || m_Token->getForRevision(authentication) != token) return;
    m_KeepaliveTimer.stop();
    m_KeepaliveHostId = hostId;
    m_LeaseRenewal.start(grant, authentication, hostId, token, grantedAtMs, expiresIn);
    scheduleKeepalive();
}

void RemoteBroker::attachKeepaliveSession(Session* session)
{
    // Attaching the prepared Session must not restart the lease clock. The
    // initial grant is already being renewed while its workstation is probed.
    m_Diagnostics->begin();
    connect(session,&Session::diagnosticStreamParameters,m_Diagnostics,&BetaDiagnostics::setStreamParameters);
    connect(session,&Session::connectionStarted,this,[this] {
        m_Diagnostics->recordEvent("connected"); m_DiagnosticSampleTimer.start();
    });
    connect(session,&Session::stageFailed,this,[this](const QString&,int code,const QString&) { m_Diagnostics->recordEvent("stage_failed",code); });
    connect(session,&Session::displayLaunchError,this,[this] { m_Diagnostics->recordEvent("display_failed"); });
    connect(session,&Session::diagnosticConnectionLost,this,[this](int code) { m_Diagnostics->recordEvent("connection_lost",code); });
    m_KeepaliveSession = session;
    m_KeepaliveOwner = session;
    connect(session, &Session::connectionStarted, this, [this, session]() {
        if (m_KeepaliveOwner != session || !m_PresenceId.isEmpty()) return;
        m_PresenceId = QUuid::createUuid().toString(QUuid::Id128);
        m_PresenceSequence = 0;
        sendPresence(true);
        m_PresenceTimer.start();
    });
    auto stopForSession = [this, session]() {
        if (m_KeepaliveOwner == session) {
            m_DiagnosticSampleTimer.stop();
            m_Diagnostics->finish();
            stopKeepalive();
            refreshHosts();
            QTimer::singleShot(6000, this, [this]() {
                if (m_KeepaliveSession.isNull()) refreshHostsImpl(false);
            });
            QTimer::singleShot(18000, this, [this]() {
                if (m_KeepaliveSession.isNull()) refreshHostsImpl(false);
            });
        }
    };
    connect(session, &Session::sessionFinished, this, stopForSession);
    connect(session, &QObject::destroyed, this, stopForSession);
}

void RemoteBroker::stopKeepalive()
{
    m_PresenceTimer.stop();
    sendPresence(false);
    m_PresenceId.clear();
    m_Token->cancelLease();
    m_KeepaliveTimer.stop();
    m_LeaseRenewal.stop();
    m_KeepaliveHostId.clear();
    m_KeepaliveSession.clear();
    m_KeepaliveOwner = nullptr;
}

void RemoteBroker::sendPresence(bool connected)
{
    if (m_PresenceId.isEmpty() || m_KeepaliveHostId.isEmpty()) return;
    const QString token = m_Token->getForRevision(m_LeaseRenewal.authentication());
    if (token.isEmpty()) return;
    const auto config = clientConfig().withFrozenDeviceProofRequirement();
    const QString host = m_KeepaliveHostId;
    const QString id = m_PresenceId;
    const quint64 sequence = ++m_PresenceSequence;
    QThreadPool::globalInstance()->start([config, token, host, id, sequence, connected]() {
        try {
            PlankBrokerClient(config).presence(token, host, id, sequence, connected);
        } catch (const PlankBrokerError&) {
            // Advisory presence expires automatically after lost heartbeats.
            // It must never interrupt streaming or transport lease renewal.
        }
    });
}

void RemoteBroker::scheduleKeepalive()
{
    const qint64 delay = m_LeaseRenewal.nextDelayMs(nowMs());
    if (delay < 0) {
        if (m_LeaseRenewal.active()) {
            qWarning() << "Remote access lease keepalive missed the granted deadline";
            reportSessionNotice(tr("Fernweh could not renew remote access to this workstation. An existing direct connection may continue; check the network and sign in again before reconnecting."));
            if (m_KeepaliveOwner == nullptr) setBusy(QString());
        }
        stopKeepalive();
        return;
    }
    m_KeepaliveTimer.start(static_cast<int>(delay));
}

void RemoteBroker::sendKeepalive()
{
    if (!m_Token->currentLease(m_LeaseRenewal.authentication(), m_LeaseRenewal.grant())) return;
    const auto pending = m_LeaseRenewal.beginRequest(nowMs());
    if (!pending) {
        scheduleKeepalive();
        return;
    }
    const auto request = *pending;
    const PlankBrokerClient::Config config = clientConfig();
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, request]() {
        int failureKind = -1;
        int expiresIn = 0;
        try {
            expiresIn = PlankBrokerClient(config).keepalive(request.token, request.hostId);
        } catch (const PlankBrokerError& error) {
            failureKind = error.kind();
        }
        QMetaObject::invokeMethod(qApp, [self, request, failureKind, expiresIn]() {
            if (!self || !self->m_Token->currentLease(request.authentication, request.grant) ||
                    !self->m_LeaseRenewal.recordResult(request, nowMs(), expiresIn)) return;
            if (failureKind == PlankBrokerError::SessionExpired ||
                    failureKind == PlankBrokerError::Denied ||
                    failureKind == PlankBrokerError::NotConfigured ||
                    failureKind == PlankBrokerError::Tls) {
                // A terminal reason is visible, while an established direct
                // stream can remain usable until the person closes it.
                qWarning() << "Remote access lease keepalive rejected; kind" << failureKind;
                const PlankBrokerError failure(static_cast<PlankBrokerError::Kind>(failureKind));
                if (failure.kind() == PlankBrokerError::SessionExpired)
                    self->signOutLocally(failure.userMessage());
                else {
                    self->reportSessionNotice(failure.kind() == PlankBrokerError::Denied ?
                        tr("Remote access to this workstation is no longer authorized. An existing direct connection may continue; ask the studio to check your access before reconnecting.") : failure.userMessage());
                    if (self->m_KeepaliveOwner == nullptr) self->setBusy(QString());
                    self->stopKeepalive();
                }
                return;
            }
            if (failureKind == PlankBrokerError::DeviceKeyUnavailable)
                self->reportSessionNotice(PlankBrokerError(PlankBrokerError::DeviceKeyUnavailable).userMessage());
            self->scheduleKeepalive();
        }, Qt::QueuedConnection);
    });
}

QObject* RemoteBroker::betaDiagnostics() const { return m_Diagnostics; }
void RemoteBroker::configureDiagnostics()
{
    if (!configured() || !signedIn()) { m_Diagnostics->setAccount(QString(),{}); return; }
    auto pins = PlankBroker::normalizePins(m_Preferences->brokerPins); pins.sort();
    const QString identity = brokerAddress().toLower()+"\n"+pins.join(",")+"\n"+m_Username;
    QSettings settings;
    if (m_Diagnostics->enabled() && !settings.value("betaDiagnosticsNoticeShown",false).toBool()) {
        const QString notice = tr("Beta diagnostics are enabled: session events, performance summaries and display/HDR settings are sent to your remote access administrator after disconnecting. You can turn this off in Settings.");
        reportSessionNotice(m_SessionNotice.isEmpty() ? notice : m_SessionNotice+"\n"+notice);
        settings.setValue("betaDiagnosticsNoticeShown",true);
    }
    const auto token = m_Token;
    const auto revision = token->revision();
    const auto config = clientConfig().withFrozenDeviceProofRequirement();
    m_Diagnostics->setAccount(identity,[token,revision,config](const QJsonObject& report,const std::shared_ptr<std::atomic_bool>& cancelled) {
        auto lowPriority = config;
        lowPriority.cancelled = [cancelled] { return cancelled->load(); };
        lowPriority.requestTimeoutMs = 3000;
        const auto current = token->getForRevision(revision);
        if (current.isEmpty() || cancelled->load()) return 0;
        return PlankBrokerClient(lowPriority).submitDiagnostics(current,report);
    });
}

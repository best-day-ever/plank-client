#include "remotebroker.h"

#include "backend/brokersessionstore.h"
#include "backend/clientdisplayprobe.h"
#include "backend/computermanager.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/onboardingstate.h"
#include "backend/outputtopology.h"
#include "backend/remotedisplaysetup.h"
#include "backend/remotestreamsetup.h"
#include "settings/streamingpreferences.h"
#include "streaming/session.h"

#include <QDebug>
#include <QGuiApplication>
#include <QSettings>
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QThreadPool>
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
    switch (error.kind()) {
    case PlankBrokerError::Network:
        throw QtNetworkReplyException(QNetworkReply::TimeoutError, error.userMessage());
    case PlankBrokerError::Tls:
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, error.userMessage());
    case PlankBrokerError::RateLimited:
        throw GfeHttpResponseException(429, error.userMessage());
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
    QString hostId;
    NvComputer* computer = nullptr;
};

}

RemoteBroker::RemoteBroker(StreamingPreferences* preferences, QObject* parent)
    : QObject(parent),
      m_Preferences(preferences),
      m_Token(std::make_shared<SharedToken>())
{
    monotonicClock();
    m_KeepaliveTimer.setSingleShot(true);
    connect(&m_KeepaliveTimer, &QTimer::timeout, this, &RemoteBroker::sendKeepalive);
    connect(m_Preferences, &StreamingPreferences::brokerChanged,
            this, &RemoteBroker::configurationChanged);
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
    // Resume a remembered session (macOS Keychain). If the broker has since
    // expired it, the first host list returns 401 and signOutLocally drops it.
    if (!signedIn() && configured()) {
        BrokerSessionStore::Saved saved;
        if (BrokerSessionStore::load(brokerAddress(), saved)) {
            m_Token->set(saved.token);
            saved.token.fill(QChar('\0'));
            m_Username = saved.username;
            emit stateChanged();
        }
    }
}

PlankBrokerClient::Config RemoteBroker::clientConfig() const
{
    PlankBrokerClient::Config config;
    config.host = m_Preferences->brokerHost;
    config.port = static_cast<quint16>(qBound(0, m_Preferences->brokerPort, 65535));
    config.pins = m_Preferences->brokerPins;
    // Device-bound sessions (bde-linux docs/plank-broker.md 14): with the
    // bundled helper (macOS) the session is bound to a Secure Enclave key
    // for this broker host and every bearer call carries a proof. No helper,
    // an IP-literal broker or any helper failure: unbound, exactly as before.
    const QString deviceHost = m_Preferences->brokerHost.trimmed().toLower();
    if (m_PasskeyHelper.available() && PlankBroker::isPasskeyRpId(deviceHost)) {
        const PlankPasskeyHelper helper(m_PasskeyHelper.program());
        config.devicePublicKey = [helper, deviceHost]() { return helper.devicePublicKey(deviceHost); };
        config.deviceSigner = [helper, deviceHost](const QByteArray& message) {
            return helper.deviceSign(deviceHost, message);
        };
    }
    return config;
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
    stopKeepalive();
    m_Token->clear();
    BrokerSessionStore::clear(brokerAddress());
    m_Username.clear();
    m_Hosts.clear();
    m_BusyText.clear();
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
    const quint64 generation = ++m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, generation, user, password, otp]() mutable {
        QString token;
        QString confirmedUser;
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
            confirmedUser = reply.username.isEmpty() ? user : reply.username;
            qInfo() << "Remote access session device-bound:" << reply.deviceBound;
        } catch (const PlankBrokerError& error) {
            failure = std::make_unique<PlankBrokerError>(error);
        }
        password.fill(QChar('\0'));
        otp.fill(QChar('\0'));
        QMetaObject::invokeMethod(qApp, [self, generation, token, confirmedUser,
                                         failure = std::shared_ptr<PlankBrokerError>(failure.release())]() mutable {
            if (!self || generation != self->m_Generation) {
                token.fill(QChar('\0'));
                return;
            }
            if (failure) {
                self->handleBrokerError(*failure, false);
                return;
            }
            self->finishSignIn(token, confirmedUser);
            token.fill(QChar('\0'));
        }, Qt::QueuedConnection);
    });
}

QString RemoteBroker::passkeyRpId() const
{
    const QString rpId = m_Preferences->passkeyRpId.trimmed().toLower();
    return PlankBroker::isPasskeyRpId(rpId) ? rpId : PlankBroker::defaultPasskeyRpId();
}

void RemoteBroker::finishSignIn(QString token, const QString& confirmedUser)
{
    m_Token->set(token);
    // Password + code and Touch ID both end here: remember the session.
    BrokerSessionStore::save(brokerAddress(), confirmedUser, token);
    token.fill(QChar('\0'));
    // Lasting proof this Mac was used before, beyond the Keychain session
    // that sign-out and an expired session clear: no wizard on next launch.
    {
        QSettings settings;
        OnboardingState::recordBrokerSignIn(settings);
    }
    m_Username = confirmedUser;
    setBusy(QString());
    emit stateChanged();
    refreshHosts();
}

void RemoteBroker::adoptSession(QString token, const QString& confirmedUser)
{
    if (token.isEmpty() || confirmedUser.isEmpty()) {
        token.fill(QChar('\0'));
        return;
    }
    // Anything still in flight belongs to the previous state.
    ++m_Generation;
    finishSignIn(token, confirmedUser);
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
        const auto result = outcome.result;
        QMetaObject::invokeMethod(qApp, [self, generation, token, confirmedUser, result, failure]() mutable {
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
                self->finishSignIn(token, confirmedUser);
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
    if (!signedIn() || busy()) return;
    setBusy(tr("Loading workstations..."));
    const PlankBrokerClient::Config config = clientConfig();
    const auto token = m_Token;
    const quint64 generation = m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, generation]() {
        QVector<PlankBroker::Host> hosts;
        std::shared_ptr<PlankBrokerError> failure;
        try {
            hosts = PlankBrokerClient(config).hosts(token->get());
        } catch (const PlankBrokerError& error) {
            failure = std::make_shared<PlankBrokerError>(error);
        }
        QMetaObject::invokeMethod(qApp, [self, generation, hosts, failure]() {
            if (!self || generation != self->m_Generation) return;
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
                                    RemoteStreamSetup::Capabilities& capabilities)
{
    const NvAddress address(lease.endpoint, lease.port);
    NvHTTP http(address);
    http.setPinnedCertificateSha256(lease.hostCertSha256);
    const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR);
    NvComputer probed(http, serverInfo);
    if (!probed.plankAuthentication) {
        throw GfeHttpResponseException(400, "The remote workstation does not offer PLANK authentication");
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

    const QString token = http.authenticateGssapi(lease.username, lease.gssapiToken);
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
        topology = macHost ? http.prepareMacDisplay(desktopMode, appleEncodingMode, 1) :
                             http.getOutputTopology();
    }
    const QVector<NvApp> apps = http.getAppList();
    {
        QWriteLocker lock(&computer->lock);
        computer->sessionToken = token;
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
    RemoteDisplaySetup::Setup display = RemoteDisplaySetup::load(settings, hostId);
    if (!display.configured) {
        emit displaySetupRequired(hostId, hostName, QString());
        return;
    }
    const QStringList modes = hostVirtualModes(settings, hostId);
    QString matchReason;
    if (!RemoteDisplaySetup::refreshMatchedModes(settings, hostId, display,
                                                 ClientDisplayProbe::probe(), &matchReason, modes)) {
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
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, generation, hostId, hostName, stream, display]() {
        NvComputer* computer = nullptr;
        std::shared_ptr<PlankBrokerError> brokerFailure;
        QString hostFailure;
        QString streamProblem;
        RemoteStreamSetup::Capabilities capabilities;
        try {
            PlankBroker::Lease lease = PlankBrokerClient(config).connect(token->get(), hostId);
            // Office LAN: the broker hands out the workstation itself; the flow is identical.
            qInfo() << "Remote access route to" << hostId << ":"
                    << (lease.route == PlankBroker::Route::Direct ? "direct" : "relay");
            try {
                computer = prepareBrokeredComputer(lease, hostId, hostName, stream, display, capabilities);
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
        QMetaObject::invokeMethod(qApp, [self, generation, hostId, hostName, computer,
                                         brokerFailure, hostFailure, streamProblem, capabilities]() {
            if (capabilities.known) {
                // Kept for the settings dialog and the next connect's check.
                QSettings settings;
                RemoteStreamSetup::saveCapabilities(settings, hostId, capabilities);
            }
            if (!self || generation != self->m_Generation) {
                // Signed out meanwhile; the one-use host token is abandoned.
                delete computer;
                return;
            }
            if (brokerFailure) {
                self->handleBrokerError(*brokerFailure, true);
                return;
            }
            if (!streamProblem.isEmpty()) {
                // The lease is abandoned; a retry is a fresh broker connect.
                self->setBusy(QString());
                emit self->streamSetupRequired(hostId, hostName, streamProblem);
                return;
            }
            if (computer == nullptr) {
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
                self->setBusy(QString());
                emit self->errorOccurred(tr("The workstation did not provide its Desktop session."));
                return;
            }

            auto context = std::make_shared<AdmissionContext>();
            context->config = self->clientConfig();
            const auto sharedToken = self->m_Token;
            context->currentToken = [sharedToken]() { return sharedToken->get(); };
            context->hostId = hostId;
            context->computer = computer;
            session->setPlankBrokerAdmission([context](QString& username, QString& gssapiToken) {
                try {
                    PlankBroker::Lease lease = PlankBrokerClient(context->config)
                            .connect(context->currentToken(), context->hostId);
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

            self->m_PendingSession = session;
            self->startKeepalive(hostId, session);
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
    const PlankBrokerClient::Config config = clientConfig();
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

void RemoteBroker::startKeepalive(const QString& hostId, Session* session)
{
    stopKeepalive();
    m_KeepaliveHostId = hostId;
    m_KeepaliveSession = session;
    m_KeepaliveSchedule.start(nowMs());
    auto stopForSession = [this, session]() {
        if (m_KeepaliveSession.isNull() || m_KeepaliveSession.data() == session) {
            stopKeepalive();
            // Show fresh availability after a stream ends.
            refreshHosts();
        }
    };
    connect(session, &Session::sessionFinished, this, stopForSession);
    connect(session, &QObject::destroyed, this, [this]() {
        if (m_KeepaliveSession.isNull()) stopKeepalive();
    });
    scheduleKeepalive();
}

void RemoteBroker::stopKeepalive()
{
    m_KeepaliveTimer.stop();
    m_KeepaliveSchedule.stop();
    m_KeepaliveHostId.clear();
    m_KeepaliveSession.clear();
}

void RemoteBroker::scheduleKeepalive()
{
    const qint64 delay = m_KeepaliveSchedule.nextDelayMs(nowMs());
    if (delay < 0) {
        if (m_KeepaliveSchedule.active()) {
            qWarning() << "Remote access lease keepalive gave up after repeated failures";
        }
        stopKeepalive();
        return;
    }
    m_KeepaliveTimer.start(static_cast<int>(delay));
}

void RemoteBroker::sendKeepalive()
{
    if (!m_KeepaliveSchedule.active() || m_KeepaliveInFlight || m_KeepaliveHostId.isEmpty()) return;
    m_KeepaliveInFlight = true;
    const PlankBrokerClient::Config config = clientConfig();
    const auto token = m_Token;
    const QString hostId = m_KeepaliveHostId;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, hostId]() {
        int failureKind = -1;
        try {
            PlankBrokerClient(config).keepalive(token->get(), hostId);
        } catch (const PlankBrokerError& error) {
            failureKind = error.kind();
        }
        QMetaObject::invokeMethod(qApp, [self, hostId, failureKind]() {
            if (!self) return;
            self->m_KeepaliveInFlight = false;
            if (!self->m_KeepaliveSchedule.active() || self->m_KeepaliveHostId != hostId) return;
            if (failureKind < 0) {
                self->m_KeepaliveSchedule.recordSuccess(nowMs());
            } else if (failureKind == PlankBrokerError::SessionExpired ||
                       failureKind == PlankBrokerError::Denied ||
                       failureKind == PlankBrokerError::NotConfigured ||
                       failureKind == PlankBrokerError::Tls) {
                // Established flows may survive, but the lease cannot be renewed.
                qWarning() << "Remote access lease keepalive rejected; kind" << failureKind;
                self->stopKeepalive();
                return;
            } else {
                self->m_KeepaliveSchedule.recordFailure();
            }
            self->scheduleKeepalive();
        }, Qt::QueuedConnection);
    });
}

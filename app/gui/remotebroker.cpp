#include "remotebroker.h"

#include "backend/computermanager.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/outputtopology.h"
#include "settings/streamingpreferences.h"
#include "streaming/session.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QThreadPool>
#include <QCoreApplication>

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
    // The direct route failed at connect time: re-admissions stay on the relay.
    bool forceRelay = false;
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
}

PlankBrokerClient::Config RemoteBroker::clientConfig() const
{
    PlankBrokerClient::Config config;
    config.host = m_Preferences->brokerHost;
    config.port = static_cast<quint16>(qBound(0, m_Preferences->brokerPort, 65535));
    config.pins = m_Preferences->brokerPins;
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
            self->m_Token->set(token);
            token.fill(QChar('\0'));
            self->m_Username = confirmedUser;
            self->setBusy(QString());
            emit self->stateChanged();
            self->refreshHosts();
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

RemoteBroker::HostDefaults RemoteBroker::bookmarkDefaultsFor(const QString& hostName) const
{
    HostDefaults defaults;
    if (m_ComputerManager.isNull()) return defaults;
    const QVector<NvComputer*> computers = m_ComputerManager->getComputers();
    for (NvComputer* computer : computers) {
        QReadLocker lock(&computer->lock);
        const QString address = computer->manualAddress.isNull() ? QString() :
                                                                 computer->manualAddress.address();
        const bool matches = computer->name.compare(hostName, Qt::CaseInsensitive) == 0 ||
                address.compare(hostName, Qt::CaseInsensitive) == 0 ||
                address.startsWith(hostName + QLatin1Char('.'), Qt::CaseInsensitive);
        if (!matches) continue;
        defaults.found = true;
        defaults.videoProfile = computer->plankVideoProfile;
        defaults.captureSource = computer->plankCaptureSource;
        defaults.scalingMode = computer->plankScalingMode;
        defaults.hostLayout = computer->plankHostLayout;
        defaults.virtualMode1 = computer->plankVirtualMode1;
        defaults.virtualMode2 = computer->plankVirtualMode2;
        defaults.profileBitratesKbps = computer->plankProfileBitratesKbps;
        break;
    }
    return defaults;
}

namespace {

// Worker thread: section 10.2 steps 1-2 plus the ordinary pre-launch
// preparation (topology, app list). The returned computer is authorized with
// a one-use host token and pinned to the broker-supplied leaf.
NvComputer* prepareBrokeredComputer(const PlankBroker::Lease& lease, const QString& hostId,
                                    const QString& hostName, bool defaultsFound, int profile, int capture,
                                    const QString& scalingMode, const QString& hostLayout,
                                    const QString& virtualMode1, const QString& virtualMode2,
                                    const QVector<int>& bitrates)
{
    const NvAddress address(lease.endpoint, lease.port);
    NvHTTP http(address);
    http.setPinnedCertificateSha256(lease.hostCertSha256);
    // Direct route: fail fast so an unreachable workstation falls back to the relay quickly.
    const QString serverInfo = http.getServerInfo(NvHTTP::NVLL_ERROR,
                                                  lease.route == PlankBroker::Route::Direct);
    NvComputer probed(http, serverInfo);
    if (!probed.plankAuthentication) {
        throw GfeHttpResponseException(400, "The remote workstation does not offer PLANK authentication");
    }
    const bool macHost = probed.plankFeatureFlags == NvOutputTopology::FixedCaptureFlags;
    const bool bookmarkMatchesPlatform = defaultsFound &&
            ((capture == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) == macHost) &&
            StreamingPreferences::isPlankProfileValidForCaptureSource(profile, capture);
    if (!bookmarkMatchesPlatform) {
        capture = macHost ? StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT :
                            StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
        profile = macHost ? StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420 :
                            StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444;
    }
    std::unique_ptr<NvComputer> computer(new NvComputer(
            address, hostName, profile, capture,
            bookmarkMatchesPlatform && !bitrates.isEmpty() ? bitrates :
                                                           StreamingPreferences::plankDefaultProfileBitrates()));
    {
        QWriteLocker lock(&computer->lock);
        if (bookmarkMatchesPlatform) {
            computer->plankScalingMode = scalingMode;
            computer->plankHostLayout = hostLayout;
            computer->plankVirtualMode1 = virtualMode1;
            computer->plankVirtualMode2 = virtualMode2;
        }
        if (macHost) {
            // Match Client needs a GUI-thread display snapshot; remote Mac
            // hosts use the bookmark's fixed virtual display instead.
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

void RemoteBroker::connectToHost(const QString& hostId)
{
    if (!signedIn() || busy() || !PlankBroker::isHostId(hostId)) return;
    QString hostName = hostId;
    for (const QVariant& value : std::as_const(m_Hosts)) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("id")).toString() == hostId) {
            hostName = entry.value(QStringLiteral("name")).toString();
        }
    }
    const HostDefaults defaults = bookmarkDefaultsFor(hostName);

    setBusy(tr("Connecting to %1...").arg(hostName));
    const PlankBrokerClient::Config config = clientConfig();
    const auto token = m_Token;
    const quint64 generation = m_Generation;
    QPointer<RemoteBroker> self(this);
    QThreadPool::globalInstance()->start([self, config, token, generation, hostId, hostName, defaults]() {
        NvComputer* computer = nullptr;
        std::shared_ptr<PlankBrokerError> brokerFailure;
        QString hostFailure;
        bool forceRelay = false;
        auto prepare = [&](PlankBroker::Lease& lease) {
            NvComputer* prepared = prepareBrokeredComputer(lease, hostId, hostName, defaults.found,
                                                           defaults.videoProfile, defaults.captureSource,
                                                           defaults.scalingMode, defaults.hostLayout,
                                                           defaults.virtualMode1, defaults.virtualMode2,
                                                           defaults.profileBitratesKbps);
            lease.gssapiToken.fill(QChar('\0'));
            return prepared;
        };
        try {
            PlankBroker::Lease lease = PlankBrokerClient(config).connect(token->get(), hostId);
            if (lease.route == PlankBroker::Route::Direct) {
                qInfo() << "Remote access: direct route to" << hostId;
                try {
                    computer = prepare(lease);
                } catch (const QtNetworkReplyException& error) {
                    // A pin mismatch is an identity failure, never a routing one.
                    if (error.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                    qWarning() << "Direct route to" << hostId << "failed, using the relay:" << error.toQString();
                    lease.gssapiToken.fill(QChar('\0'));
                    forceRelay = true;
                }
            }
            if (computer == nullptr) {
                if (forceRelay) lease = PlankBrokerClient(config).connect(token->get(), hostId, true);
                computer = prepare(lease);
            }
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
                                         brokerFailure, hostFailure, forceRelay]() {
            if (!self || generation != self->m_Generation) {
                // Signed out meanwhile; the one-use host token is abandoned.
                delete computer;
                return;
            }
            if (brokerFailure) {
                self->handleBrokerError(*brokerFailure, true);
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
            context->forceRelay = forceRelay;
            session->setPlankBrokerAdmission([context](QString& username, QString& gssapiToken) {
                try {
                    PlankBroker::Lease lease = PlankBrokerClient(context->config)
                            .connect(context->currentToken(), context->hostId, context->forceRelay);
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

#include "nvcomputer.h"
#include "desktopstage.h"
#include "hostrecovery.h"
#include "plankbroker.h"
#include "plankhttp.h"
#include <QCryptographicHash>
#include <QScopedPointer>
#include <Limelight.h>

#include <utility>
#include <memory>

#include <QDebug>
#include <QDateTime>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QSslCipher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000

namespace {
class SecureStringGuard
{
public:
    explicit SecureStringGuard(QString& value) : m_Value(value) {}
    ~SecureStringGuard()
    {
        m_Value.fill(QChar('\0'));
        m_Value.clear();
    }

private:
    QString& m_Value;
};

QSslConfiguration plankSslConfiguration()
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setProtocol(QSsl::TlsV1_3OrLater);
    return configuration;
}

QSslConfiguration negotiatedPlankTls(QNetworkReply* reply)
{
    return HostTlsGuard::negotiated(reply);
}
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this))
{
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setTrustAddress(address);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(computer->activeAddress, nam)
{
    setPlankSessionToken(computer->sessionToken, computer->sessionIdentityKey);
    setPinnedCertificateSha256(computer->brokerHostCertSha256);
}

void NvHTTP::setPinnedCertificateSha256(QString certificateSha256)
{
    // An invalid, non-empty pin still marks the host as brokered, so every
    // certificate is then rejected (fail closed) rather than falling back.
    m_PinnedCertificateSha256 = std::move(certificateSha256);
}

bool NvHTTP::acceptsPlankCertificate(const QSslCertificate& certificate) const
{
    if (!isPlankCertificate(certificate)) {
        return false;
    }
    return m_PinnedCertificateSha256.isEmpty() ||
            PlankBroker::hostLeafMatches(certificate.toDer(), m_PinnedCertificateSha256);
}

QMetaObject::Connection NvHTTP::enforcePinnedCertificate(QNetworkAccessManager* manager)
{
    if (m_PinnedCertificateSha256.isEmpty()) {
        return {};
    }
    // Re-check at handshake completion, before any request bytes (including
    // a bearer token) are written, even if the TLS stack raised no errors.
    return connect(manager, &QNetworkAccessManager::encrypted, this, [this](QNetworkReply* reply) {
        if (!acceptsPlankCertificate(reply->sslConfiguration().peerCertificate())) {
            qWarning() << "Rejecting a brokered host certificate that does not match the broker pin";
            reply->setProperty("plankPinRejected", true);
            reply->abort();
        }
    });
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttps.setHost(address.address());
    m_BaseUrlHttps.setPort(address.port());
}

void NvHTTP::setTrustAddress(NvAddress address)
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(address.address());
    url.setPort(address.port());
    m_TrustEndpoint = HostTrustStore::endpoint(url);
}

void NvHTTP::setPlankSessionToken(QString sessionToken, QByteArray identityKey)
{
    m_SessionToken = std::move(sessionToken);
    m_IdentityKey = std::move(identityKey);
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

uint16_t NvHTTP::controlPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    const QString serverInfo = openConnectionToString(
                m_BaseUrlHttps,
                "serverinfo",
                nullptr,
                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                logLevel);
    verifyResponseStatus(serverInfo);
    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString captureDisplayMode,
                 QString topologyGeneration,
                 int plankProtocolVersion,
                 int plankFeatureFlags,
                 bool takeOverActiveSession,
                 QString desktopSignOutOwner,
                 QString hostLayout,
                 QString virtualMode1,
                 QString virtualMode2,
                 QString displayArrangement,
                 QString captureSource,
                 QString encoderBackend,
                 QString encodingMode,
                 quint16 quicUdpPayloadMtu,
                 quint16& plankTransportPort,
                 QString& plankTransportCertificateSha256,
                 QString& plankTransportToken,
                 QString& acceptedCaptureSource,
                 QString& acceptedEncoderBackend,
                 QString& acceptedEncodingMode,
                 QString& acceptedFileClipboardMode,
                 int primaryOutput)
{
    QString plankOutputArguments;
    if (!captureDisplayMode.isEmpty()) {
        plankOutputArguments =
                "&plankProtocolVersion=" + QString::number(plankProtocolVersion) +
                "&plankFeatureFlags=" + QString::number(plankFeatureFlags) +
                "&plankDisplayMode=" + QString::fromLatin1(QUrl::toPercentEncoding(captureDisplayMode));
        if (takeOverActiveSession &&
                (plankFeatureFlags & NvOutputTopology::SessionTakeoverFeature) != 0) {
            plankOutputArguments += "&plankTakeover=1";
        }
        // The owner must match the host's offer exactly; the host re-checks
        // that nobody is streaming that desktop before it signs it out.
        if (!desktopSignOutOwner.isEmpty() &&
                (plankFeatureFlags & NvOutputTopology::DesktopSignOutFeature) != 0) {
            plankOutputArguments +=
                    "&plankSignOutDesktop=1&plankSignOutOwner=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(desktopSignOutOwner));
        }
        plankOutputArguments +=
                "&plankCaptureSource=" +
                QString::fromLatin1(QUrl::toPercentEncoding(captureSource));
        plankOutputArguments +=
                "&plankEncoderBackend=" +
                QString::fromLatin1(QUrl::toPercentEncoding(encoderBackend));
        plankOutputArguments +=
                "&plankEncodingMode=" +
                QString::fromLatin1(QUrl::toPercentEncoding(encodingMode));
        if ((plankFeatureFlags &
             NvOutputTopology::FixedTransportMtuFeature) != 0) {
            plankOutputArguments +=
                    "&plankQuicUdpPayloadMtu=" +
                    QString::number(quicUdpPayloadMtu);
        }
        if ((plankFeatureFlags & NvOutputTopology::DisplayArrangementFeature) != 0 &&
                !displayArrangement.isEmpty()) {
            // Replaces the host layout and virtual modes; the host refuses
            // a launch that carries both.
            plankOutputArguments +=
                    "&plankDisplayArrangement=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(displayArrangement));
        }
        else if ((plankFeatureFlags & NvOutputTopology::HostLayoutBindingFeature) != 0 &&
                !hostLayout.isEmpty()) {
            plankOutputArguments +=
                    "&plankHostLayout=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(hostLayout));
            if (primaryOutput >= 0 &&
                    (plankFeatureFlags & NvOutputTopology::VirtualPrimaryConnectorFeature)) {
                plankOutputArguments += "&plankPrimaryOutput=" + QString::number(primaryOutput);
            }
            if ((plankFeatureFlags &
                    NvOutputTopology::IndependentVirtualModesFeature) != 0) {
                if (!virtualMode1.isEmpty()) {
                    plankOutputArguments +=
                            "&plankVirtualMode1=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode1));
                }
                if (!virtualMode2.isEmpty()) {
                    plankOutputArguments +=
                            "&plankVirtualMode2=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode2));
                }
            }
        }
        if ((plankFeatureFlags & NvOutputTopology::TopologyGenerationFeature) != 0 &&
                !topologyGeneration.isEmpty()) {
            plankOutputArguments +=
                    "&plankTopologyGeneration=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(topologyGeneration));
        }
    }

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   QString::number(streamConfig->fps)+
                                   "&additionalStates=1"+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   plankOutputArguments,
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "PLANK launch response received";

    m_DisplayArrangementError.clear();
    if ((plankFeatureFlags & NvOutputTopology::DisplayArrangementFeature) != 0) {
        // A short code from the contract; anything else is not trusted as one.
        const QString code = getXmlString(response, "PlankDisplayArrangementError").trimmed();
        static const QRegularExpression codeShape(QStringLiteral("^[a-z_]{1,32}$"));
        if (codeShape.match(code).hasMatch()) {
            m_DisplayArrangementError = code;
        }
    }
    m_DesktopSignOut = {};
    if ((plankFeatureFlags & NvOutputTopology::DesktopSignOutFeature) != 0) {
        QXmlStreamReader xmlReader(response);
        if (xmlReader.readNextStartElement() && XML_NAME_EQUALS(xmlReader.name(), "root")) {
            m_DesktopSignOut = PlankDesktopSignOut::fromResponse(
                        (int)xmlReader.attributes().value("status_code").toUInt(),
                        getXmlString(response, "PlankDesktopOwner"),
                        getXmlString(response, "PlankDesktopSignOut"));
        }
    }

    // Throws if the request failed
    verifyResponseStatus(response);

    m_WorkerInstance = PlankHostRecovery::canonicalInstance(getXmlString(response, "PlankWorkerInstance"));
    if ((plankFeatureFlags & NvOutputTopology::WorkerInstanceFeature) && m_WorkerInstance.isEmpty()) {
        throw GfeHttpResponseException(400, "Host returned an invalid media-worker identity");
    }

    plankTransportPort = getXmlString(response, "PlankTransportPort").toUShort();
    plankTransportCertificateSha256 =
            getXmlString(response, "PlankTransportCertificateSha256");
    plankTransportToken = getXmlString(response, "PlankTransportToken");
    const quint16 acceptedQuicUdpPayloadMtu =
            getXmlString(response, "PlankQuicUdpPayloadMtu").toUShort();
    acceptedCaptureSource = getXmlString(response, "PlankCaptureSource");
    acceptedEncoderBackend = getXmlString(response, "PlankEncoderBackend");
    acceptedEncodingMode = getXmlString(response, "PlankEncodingMode");
    acceptedFileClipboardMode = getXmlString(response, "PlankFileClipboardMode");
    const auto isCanonicalSha256Hex = [](const QString& value) {
        const QByteArray encoded = value.toLatin1();
        const QByteArray decoded = QByteArray::fromHex(encoded);
        return encoded.size() == 64 && decoded.size() == 32 &&
                decoded.toHex() == encoded.toLower();
    };
    if (plankTransportPort == 0 ||
            !isCanonicalSha256Hex(plankTransportCertificateSha256) ||
            !isCanonicalSha256Hex(plankTransportToken)) {
        throw GfeHttpResponseException(
                    400, "Host returned invalid plank_transport launch credentials");
    }
    if (acceptedQuicUdpPayloadMtu != quicUdpPayloadMtu) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the fixed QUIC UDP payload ceiling");
    }
    if (acceptedCaptureSource.isEmpty() || acceptedCaptureSource != captureSource) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested capture source");
    }
    if (acceptedEncoderBackend.isEmpty() || acceptedEncoderBackend != encoderBackend) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested encoder backend");
    }
    if (acceptedEncodingMode.isEmpty() || acceptedEncodingMode != encodingMode) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested encoding mode");
    }
    if (acceptedFileClipboardMode != QStringLiteral("off") &&
            acceptedFileClipboardMode != QStringLiteral("client-to-host") &&
            acceptedFileClipboardMode != QStringLiteral("host-to-client") &&
            acceptedFileClipboardMode != QStringLiteral("bidirectional")) {
        throw GfeHttpResponseException(
                    400, "Host returned an invalid file clipboard policy");
    }
    const bool filesNegotiated =
            (plankFeatureFlags & (NvOutputTopology::ClipboardSyncFeature |
                                  NvOutputTopology::ClipboardFilesFeature)) ==
                (NvOutputTopology::ClipboardSyncFeature |
                 NvOutputTopology::ClipboardFilesFeature);
    if (filesNegotiated != (acceptedFileClipboardMode != QStringLiteral("off"))) {
        throw GfeHttpResponseException(
                    400, "Host returned inconsistent file clipboard negotiation");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (!modes.isEmpty()) {
                if (XML_NAME_EQUALS(name, "Width")) {
                    modes.last().width = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "Height")) {
                    modes.last().height = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "RefreshRate")) {
                    modes.last().refreshRate = xmlReader.readElementText().toInt();
                }
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    throw std::runtime_error("Invalid applist XML");
                }
                apps.append(NvApp());
            }
            else if (!apps.isEmpty()) {
                if (XML_NAME_EQUALS(name, "AppTitle")) {
                    // If an app has no name, Sunshine may send us <AppTitle/>,
                    // which readElementText() returns as a null QString.
                    // We want to treat this as an empty QString instead, so we
                    // will explicitly convert it. An empty string will satisfy
                    // NvApp's isInitialized() check.
                    QString name = xmlReader.readElementText();
                    if (name.isNull()) {
                        name = "";
                    }
                    apps.last().name = name;
                }
                else if (XML_NAME_EQUALS(name, "ID")) {
                    apps.last().id = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "IsHdrSupported")) {
                    apps.last().hdrSupported = xmlReader.readElementText() == "1";
                }
                else if (XML_NAME_EQUALS(name, "IsAppCollectorGame")) {
                    apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
                }
            }
        }
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (XML_NAME_EQUALS(xmlReader.name(), "root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected before PAM authorization establishes a bearer session.
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    return QByteArray::fromHex(getXmlString(xml, tagName).toUtf8());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return QString();
}

void NvHTTP::checkTlsGuard(const HostTlsGuard& guard)
{
    const auto& result = guard.result();
    if (result.status == HostTrustStore::Status::Changed) {
        throw HostIdentityChangedException(m_TrustEndpoint, result.previousKey, guard.key());
    }
    if (!result.error.isEmpty() || result.status == HostTrustStore::Status::Unknown) {
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
            result.error.isEmpty() ? QStringLiteral("Host identity is not established. Sign in again.") : result.error);
    }
}

void NvHTTP::establishHostTrust(AuthenticationIntent intent)
{
    if (isBrokered()) {
        // The broker supplies an exact leaf pin. Observe that validated TLS
        // identity before sending credentials, without enrolling local TOFU.
        QScopedPointer<QNetworkReply> reply(openConnection(m_BaseUrlHttps, "serverinfo", {},
            REQUEST_TIMEOUT_MS, NVLL_NONE, HostTlsGuard::Mode::Observe));
        verifyResponseStatus(QString::fromUtf8(reply->readAll()));
        m_IdentityKey = HostTlsGuard::identityKey(negotiatedPlankTls(reply.data()).peerCertificateChain());
        if (m_IdentityKey.size() != 32)
            throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, "Brokered Host identity was rejected.");
        return;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            QScopedPointer<QNetworkReply> reply(openConnection(m_BaseUrlHttps, "serverinfo", {},
                REQUEST_TIMEOUT_MS, NVLL_NONE, intent == AuthenticationIntent::ExplicitConnection ?
                    HostTlsGuard::Mode::Enroll : HostTlsGuard::Mode::RequireKnown));
            verifyResponseStatus(QString::fromUtf8(reply->readAll()));
            m_IdentityKey = HostTlsGuard::identityKey(negotiatedPlankTls(reply.data()).peerCertificateChain());
            return;
        } catch (const HostIdentityChangedException& change) {
            // Only before auth/start; never resume a PAM conversation or carry
            // an old bearer token across replacement approval. The worker may
            // wait for consent, never a Qt SSL callback or the GUI thread.
            if (attempt || !m_TrustPrompt || m_RequestGate || intent != AuthenticationIntent::ExplicitConnection) throw;
            if (!m_TrustPrompt(change))
                throw QtNetworkReplyException(QNetworkReply::OperationCanceledError, "Host identity replacement cancelled.");
            const auto result = m_TrustStore.replace(change.endpoint, change.previousKey, change.replacementKey);
            if (result.status != HostTrustStore::Status::Trusted)
                throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, result.error);
        }
    }
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

QJsonObject NvHTTP::postPlankJson(QString command, const QJsonObject& body)
{
    waitForRequestPermission(true);
    if (!m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK authentication state");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/plank/auth/" + command);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setSslConfiguration(plankSslConfiguration());
    PlankHttp::prepareOneShotRequest(request);
    // Never reuse a socket for credentials: the previous response's connection
    // may already be closing, and a reused socket skips the pin check below.
    m_Nam->clearAccessCache();

    std::unique_ptr<HostTlsGuard> trustGuard;
    if (!isBrokered())
        trustGuard = std::make_unique<HostTlsGuard>(*m_Nam, m_TrustStore, m_TrustEndpoint,
            HostTlsGuard::Mode::RequireKnown, m_IdentityKey);
    const auto sslErrorsConnection = isBrokered() ? connect(
        m_Nam, &QNetworkAccessManager::sslErrors,
        this, &NvHTTP::handleSslErrors) : QMetaObject::Connection();
    const auto encryptedConnection = rememberPlankTls(m_Nam, this);
    const auto pinConnection = enforcePinnedCertificate(m_Nam);
    QScopedPointer<QNetworkReply> reply(m_Nam->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact)));
    QEventLoop loop;
    connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &loop, &QEventLoop::quit);
    QTimer::singleShot(REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) {
        reply->abort();
    }
    m_Nam->clearAccessCache();
    disconnect(sslErrorsConnection);
    disconnect(encryptedConnection);
    if (pinConnection) disconnect(pinConnection);
    if (trustGuard) {
        checkTlsGuard(*trustGuard);
        if (!trustGuard->checked())
            throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, "PLANK TLS identity was not validated");
    }
    if (reply->property("plankPinRejected").toBool()) {
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
                                      "PLANK TLS validation failed");
    }
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = reply->error();
        const QString message = reply->errorString();
        if (status >= 400) throw GfeHttpResponseException(status, "PLANK authentication request rejected");
        throw QtNetworkReplyException(error, message);
    }
    const QSslConfiguration negotiatedSsl = negotiatedPlankTls(reply.data());
    if (!acceptsPlankCertificate(negotiatedSsl.peerCertificate()) ||
            negotiatedSsl.sessionProtocol() != QSsl::TlsV1_3) {
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
                                      "PLANK TLS validation failed");
    }
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) throw GfeHttpResponseException(status, "PLANK authentication request rejected");
    if (!document.isObject()) {
        throw GfeHttpResponseException(400, "Malformed PLANK authentication response");
    }
    return document.object();
}

bool NvHTTP::probeWorkerReplacement(const QString& instance, const QString& certificateSha256)
{
    // Use an address-only NvHTTP, never a bearer token or PAM credentials.
    if (!m_SessionToken.isEmpty()) return false;
    QScopedPointer<QNetworkReply> reply(openConnection(m_BaseUrlHttps, "serverinfo", nullptr,
                                                      1000, NvLogLevel::NVLL_NONE));
    const QByteArray certificate = negotiatedPlankTls(reply.data()).peerCertificate().digest(QCryptographicHash::Sha256);
    const QString response = QString::fromUtf8(reply->readAll());
    verifyResponseStatus(response);
    return PlankHostRecovery::replacementConfirmed(instance,
                getXmlString(response, "PlankWorkerInstance"),
                QByteArray::fromHex(certificateSha256.toLatin1()), certificate);
}

QString NvHTTP::authenticate(QString username, QString password, bool* greeterConfirmed, AuthenticationIntent intent)
{
    if (greeterConfirmed != nullptr) *greeterConfirmed = false;
    SecureStringGuard passwordGuard(password);
    if (!m_SessionToken.isEmpty() || username.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK authentication state");
    }

    // A recovered session already has an expected identity. Its credential-free
    // preflight may observe a worker change, but must not learn a new authority.
    establishHostTrust(intent);

    QJsonObject result = postPlankJson("start", {{"username", username}});
    for (int round = 0; round < 16; ++round) {
        const QString state = result.value("state").toString();
        if (state == "authenticated") {
            m_SessionToken = result.value("session_token").toString();
            if (m_SessionToken.isEmpty()) {
                throw GfeHttpResponseException(401, "Authentication returned no session token");
            }
            if (greeterConfirmed != nullptr) {
                *greeterConfirmed = plankAuthenticatedGreeter(result);
            }
            return m_SessionToken;
        }
        if (state == "denied") {
            throw GfeHttpResponseException(401, "Operating-system authentication failed");
        }
        if (state == "busy") {
            throw GfeHttpResponseException(503, "Host authentication is busy. Please try again shortly.");
        }
        if (state != "challenge" || !result.value("messages").isArray()) {
            throw GfeHttpResponseException(400, "Invalid PAM conversation response");
        }

        QJsonArray responses;
        const QJsonArray messages = result.value("messages").toArray();
        for (const QJsonValue& value : messages) {
            const QJsonObject message = value.toObject();
            switch (message.value("style").toInt()) {
            case 1: // PAM_PROMPT_ECHO_OFF
                responses.append(password);
                break;
            case 2: // PAM_PROMPT_ECHO_ON
                responses.append(username);
                break;
            case 3: // PAM_ERROR_MSG
            case 4: // PAM_TEXT_INFO
                responses.append(QString());
                break;
            default:
                throw GfeHttpResponseException(400, "Unsupported PAM prompt style");
            }
        }
        result = postPlankJson("respond", {
            {"conversation_id", result.value("conversation_id").toString()},
            {"responses", responses},
        });
    }

    throw GfeHttpResponseException(400, "PAM conversation exceeded the round limit");
}

QString NvHTTP::authenticateGssapi(QString username, QString gssapiToken, bool* greeterConfirmed)
{
    if (greeterConfirmed != nullptr) *greeterConfirmed = false;
    SecureStringGuard tokenGuard(gssapiToken);
    if (!isBrokered() || !m_SessionToken.isEmpty() || username.isEmpty() || gssapiToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK brokered authentication state");
    }

    establishHostTrust(AuthenticationIntent::Recovery);

    const QJsonObject result = postPlankJson("start", {
        {"username", username},
        {"gssapi_token", gssapiToken},
    });
    QString token;
    switch (PlankBroker::parseHostAdmission(result, token)) {
    case PlankBroker::AdmissionResult::Authenticated:
        m_SessionToken = token;
        if (greeterConfirmed != nullptr) {
            *greeterConfirmed = plankAuthenticatedGreeter(result);
        }
        return m_SessionToken;
    case PlankBroker::AdmissionResult::Denied:
        // The broker token is one-use; the caller must go back to the broker.
        throw GfeHttpResponseException(401, "Remote workstation admission was denied");
    case PlankBroker::AdmissionResult::Busy:
        throw GfeHttpResponseException(503, "Host authentication is busy. Please try again shortly.");
    case PlankBroker::AdmissionResult::Malformed:
    default:
        throw GfeHttpResponseException(400, "Invalid brokered admission response");
    }
}

NvOutputTopology NvHTTP::getOutputTopology(QString* certificateSha256)
{
    if (certificateSha256 != nullptr) certificateSha256->clear();
    if (m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid PLANK topology state");
    }
    QScopedPointer<QNetworkReply> reply(openConnection(
                m_BaseUrlHttps, "plank/topology", nullptr,
                REQUEST_TIMEOUT_MS, NvLogLevel::NVLL_VERBOSE));
    const QString response = QString::fromUtf8(reply->readAll());
    const QJsonDocument document = QJsonDocument::fromJson(response.toUtf8());
    if (!document.isObject() && response.trimmed().startsWith(QLatin1Char('<'))) {
        // GameStream authorization failures use an XML status envelope even
        // for this PLANK JSON endpoint. This is expected after a
        // display transition replaces the media worker and its in-memory
        // bearer sessions. Preserve the 401 so the bounded transition loop
        // can authenticate once to the replacement worker.
        verifyResponseStatus(response);
    }
    if (document.isObject() && NvOutputTopology::temporarilyEmpty(document.object())) {
        throw GfeHttpResponseException(425, "Host display outputs are becoming ready");
    }
    NvOutputTopology topology;
    QString error;
    if (!document.isObject() ||
            !NvOutputTopology::fromJson(document.object(), topology, &error)) {
        throw GfeHttpResponseException(400,
                                       error.isEmpty() ?
                                           "Malformed PLANK topology response" : error);
    }
    if (certificateSha256 != nullptr) {
        *certificateSha256 = QString::fromLatin1(negotiatedPlankTls(reply.data())
                .peerCertificate().digest(QCryptographicHash::Sha256).toHex());
    }
    return topology;
}

MacPreviewLaunch::Reply NvHTTP::startMacPreview(const NvOutputTopology& topology,
                                              const QString& certificateSha256,
                                              int bitrateKbps, int udpPayloadSize)
{
    const auto body = MacPreviewLaunch::request(topology, bitrateKbps, udpPayloadSize);
    // One-shot launch: even an ambiguous timeout must require fresh auth.
    SecureStringGuard tokenGuard(m_SessionToken);
    const auto object = postPinnedMacJson(QStringLiteral("/plank/launch"), body, certificateSha256);
    MacPreviewLaunch::Reply parsed;
    if (!MacPreviewLaunch::parseReply(object, topology, controlPort(), udpPayloadSize, parsed,
                                      isBrokered())) {
        throw GfeHttpResponseException(400, "Invalid Mac preview launch response");
    }
    return parsed;
}

NvOutputTopology NvHTTP::prepareMacDisplay(const QString& mode, const QString& encodingMode, int scale,
                                         const QString& takeoverSessionId)
{
    const QSize size = NvOutputTopology::macDisplayModeSize(mode);
    auto request = NvOutputTopology::macDisplayRequest(mode, encodingMode, scale);
    if (request.isEmpty()) {
        throw GfeHttpResponseException(400, "Unsupported Mac desktop resolution");
    }
    if (!takeoverSessionId.isEmpty()) request.insert(QStringLiteral("takeover_session_id"), takeoverSessionId);
    QString pin;
    const auto current = getOutputTopology(&pin);
    if (current.featureFlags != NvOutputTopology::FixedCaptureFlags) {
        throw GfeHttpResponseException(400, "Host does not support Mac desktop preparation");
    }
    const auto object = postPinnedMacJson(QStringLiteral("/plank/display"), request, pin);
    NvOutputTopology result;
    if (!NvOutputTopology::fromJson(object, result) ||
            result.featureFlags != NvOutputTopology::FixedCaptureFlags ||
            result.desktopWidth != size.width() || result.desktopHeight != size.height() ||
            result.captureLogicalBounds.size() != QSizeF(size.width() / scale, size.height() / scale) ||
            result.appleEncodingMode != encodingMode) {
        throw GfeHttpResponseException(400, "Mac desktop did not reach the requested resolution");
    }
    return result;
}

QJsonObject NvHTTP::postPinnedMacJson(const QString& path, const QJsonObject& body,
                                    const QString& certificateSha256)
{
    waitForRequestPermission();
    const QByteArray pin = QByteArray::fromHex(certificateSha256.toLatin1());
    if ((path != QLatin1String("/plank/launch") && path != QLatin1String("/plank/display")) ||
            body.isEmpty() || pin.size() != 32 ||
            QString::fromLatin1(pin.toHex()) != certificateSha256 ||
            m_SessionToken.isEmpty() || m_SessionToken.size() > 512 ||
            m_BaseUrlHttps.scheme() != QLatin1String("https") ||
            !m_BaseUrlHttps.userInfo().isEmpty() || m_BaseUrlHttps.port(0) == 0) {
        throw GfeHttpResponseException(400, "Invalid Mac preview launch state");
    }
    for (const QChar character : m_SessionToken) {
        if (character.unicode() < 33 || character.unicode() > 126) {
            throw GfeHttpResponseException(400, "Invalid Mac preview authorization");
        }
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_SessionToken.toLatin1());
    request.setSslConfiguration(plankSslConfiguration());
    PlankHttp::prepareOneShotRequest(request);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    // A fresh manager guarantees the TLS encrypted signal before sending data;
    // reused connections are not guaranteed to emit it. Never send the bearer
    // token to a replacement certificate merely because it has PLANK's shape.
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    if (m_IdentityKey.size() != 32)
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, "Host identity is not established.");
    HostTlsGuard guard(manager, m_TrustStore, m_TrustEndpoint,
                       HostTlsGuard::Mode::RequireKnown, m_IdentityKey, pin);
    QScopedPointer<QNetworkReply> reply(manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)));
    constexpr qint64 MaximumReplyBytes = 32768;
    reply->setReadBufferSize(MaximumReplyBytes + 1);
    QByteArray response;
    bool oversized = false;
    auto drain = [&]() {
        response += reply->read(MaximumReplyBytes + 1 - response.size());
        if (response.size() > MaximumReplyBytes) {
            oversized = true;
            reply->abort();
        }
    };
    QEventLoop loop;
    connect(reply.data(), &QNetworkReply::readyRead, &loop, drain);
    connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    QTimer::singleShot(path == QLatin1String("/plank/display") ? 10000 : REQUEST_TIMEOUT_MS,
                      &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) reply->abort();
    if (!oversized) drain();
    checkTlsGuard(guard);
    if (!guard.checked() &&
            reply->error() != QNetworkReply::NoError &&
            reply->error() != QNetworkReply::SslHandshakeFailedError) {
        throw QtNetworkReplyException(reply->error(), "Mac control connection failed or timed out");
    }
    if (!guard.checked()) {
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
                                      "Mac preview TLS certificate changed or was rejected");
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (oversized) throw GfeHttpResponseException(400, "Mac preview response exceeded its size limit");
    if (status != 200 && status != 0) {
        // Do not expose arbitrary server text, redirect URLs, or response tokens.
        const auto failure = QJsonDocument::fromJson(response).object();
        response.fill('\0');
        const QString activeSession = macActiveSessionId(status, failure);
        if (!activeSession.isEmpty()) throw MacSessionActiveException(activeSession);
        if (status == 409 && failure.value(QStringLiteral("error")) == QLatin1String("session_changed")) {
            throw GfeHttpResponseException(status, "The active PLANK session changed. Connect again to confirm takeover.");
        }
        if (status == 403 && failure.value(QStringLiteral("state")) == QLatin1String("denied") &&
                failure.value(QStringLiteral("error")) == QLatin1String("host_permissions_required")) {
            throw GfeHttpResponseException(status,
                "PLANK Host requires macOS permissions. On the Mac, open PLANK Host in Applications "
                "and approve Screen Recording and Accessibility in System Settings > Privacy & Security. "
                "If already enabled, the permissions may belong to an earlier signed build.");
        }
        throw GfeHttpResponseException(status, path == QLatin1String("/plank/display") ?
            "Mac desktop resolution change was not accepted" : "Mac stream launch was not accepted");
    }
    if (reply->error() != QNetworkReply::NoError) {
        throw QtNetworkReplyException(reply->error(), "Mac preview launch failed or timed out");
    }
    QJsonParseError parseError {};
    const auto document = QJsonDocument::fromJson(response, &parseError);
    response.fill('\0');
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw GfeHttpResponseException(400, "Invalid Mac control response");
    }
    return document.object();
}

void NvHTTP::waitForRequestPermission(bool authenticating)
{
    if (m_RequestGate && !m_RequestGate(authenticating)) {
        throw QtNetworkReplyException(QNetworkReply::OperationCanceledError, "PLANK reconnect cancelled");
    }
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel, HostTlsGuard::Mode trustMode)
{
    waitForRequestPermission();
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Only operation parameters belong in the query. PLANK authorization is
    // carried separately; discovery and topology need no client ID/cache nonce.
    url.setQuery(arguments);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    if (baseUrl.scheme() != QLatin1String("https") ||
            (!m_SessionToken.isEmpty() && m_IdentityKey.size() != 32))
        throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, "Host identity is not established.");

    if (baseUrl.scheme() == "https") {
        request.setSslConfiguration(plankSslConfiguration());
        if (!m_SessionToken.isEmpty()) {
            request.setRawHeader("Authorization", "Bearer " + m_SessionToken.toUtf8());
        }
    }

    // No HTTP/2 and no persistent connections: the PLANK host closes after each response.
    PlankHttp::prepareOneShotRequest(request);

    std::unique_ptr<HostTlsGuard> trustGuard;
    if (!isBrokered())
        trustGuard = std::make_unique<HostTlsGuard>(*m_Nam, m_TrustStore, m_TrustEndpoint,
            trustMode, m_IdentityKey);
    auto sslErrorsConnection = isBrokered() ? connect(m_Nam, &QNetworkAccessManager::sslErrors,
        this, &NvHTTP::handleSslErrors) : QMetaObject::Connection();
    const auto encryptedConnection = rememberPlankTls(m_Nam, this);
    const auto pinConnection = enforcePinnedCertificate(m_Nam);
    QScopedPointer<QNetworkReply> reply(m_Nam->get(request));

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    // If we couldn't use fine-grained connection idle timeouts, kill them all now
    m_Nam->clearAccessCache();
#endif
    disconnect(sslErrorsConnection);

    disconnect(encryptedConnection);
    if (pinConnection) disconnect(pinConnection);

    if (trustGuard) {
        if (trustMode != HostTlsGuard::Mode::Observe) checkTlsGuard(*trustGuard);
        if (!trustGuard->checked())
            throw QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError, "PLANK TLS identity was not validated");
    }

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error()
                       << reply->errorString();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError ||
                reply->property("plankPinRejected").toBool()) {
            QtNetworkReplyException exception(QNetworkReply::SslHandshakeFailedError, "PLANK TLS validation failed");
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            throw exception;
        }
        else {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 400 && (command == QLatin1String("plank/topology") ||
                                 command == QLatin1String("applist"))) {
                throw GfeHttpResponseException(status, "PLANK desktop readiness request rejected");
            }
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            throw exception;
        }
    }

    const bool plankTls = baseUrl.scheme() == "https";
    const bool approvedCertificate = !plankTls ||
            acceptsPlankCertificate(negotiatedPlankTls(reply.data()).peerCertificate());
    const bool approvedProtocol = !plankTls ||
            negotiatedPlankTls(reply.data()).sessionProtocol() == QSsl::TlsV1_3;
    if (!approvedCertificate || !approvedProtocol) {
        qWarning() << "Rejecting PLANK TLS session"
                   << "certificate" << approvedCertificate
                   << "tls13" << approvedProtocol
                   << "protocol" << reply->sslConfiguration().sessionProtocol()
                   << "cipherProtocol" << reply->sslConfiguration().sessionCipher().protocol();
        QtNetworkReplyException exception(QNetworkReply::SslHandshakeFailedError, "Invalid PLANK TLS session");
        throw exception;
    }

    return reply.release();
}

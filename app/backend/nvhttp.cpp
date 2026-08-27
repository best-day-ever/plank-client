#include "nvcomputer.h"
#include <Limelight.h>

#include <utility>

#include <QDebug>
#include <QDateTime>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
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

bool isStationConnectCertificate(const QSslCertificate& certificate)
{
    const auto alternativeNames = certificate.subjectAlternativeNames();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return !certificate.isNull() && certificate.isSelfSigned() &&
            certificate.publicKey().algorithm() == QSsl::Rsa &&
            certificate.publicKey().length() >= 3072 &&
            !alternativeNames.values(QSsl::DnsEntry).isEmpty() &&
            alternativeNames.values(QSsl::IpAddressEntry).isEmpty() &&
            certificate.effectiveDate() <= now && certificate.expiryDate() > now;
}

QSslConfiguration stationConnectSslConfiguration()
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setProtocol(QSsl::TlsV1_3OrLater);
    return configuration;
}
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort,
               QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this))
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(computer->activeAddress, computer->activeHttpsPort, nam)
{
    setStationConnectAuthentication(computer->stationConnectAuthentication,
                                    computer->sessionToken);
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

void NvHTTP::setStationConnectAuthentication(bool enabled, QString sessionToken)
{
    m_StationConnectAuthentication = enabled;
    m_SessionToken = std::move(sessionToken);
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
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
    QString serverInfo;

    if (m_StationConnectAuthentication && httpsPort() != 0)
    {
        try
        {
            // Authenticated StationConnect status is available only over HTTPS.
            serverInfo = openConnectionToString(m_BaseUrlHttps,
                                                "serverinfo",
                                                nullptr,
                                                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                logLevel);
            // Throws if the request failed
            verifyResponseStatus(serverInfo);
        }
        catch (const GfeHttpResponseException&)
        {
            throw;
        }
    }
    else
    {
        // Initial discovery uses HTTP only to learn the HTTPS port and
        // StationConnect protocol marker.
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        verifyResponseStatus(serverInfo);

        // Populate the HTTPS port
        uint16_t httpsPort = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (httpsPort == 0) {
            httpsPort = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(httpsPort);

        if (m_StationConnectAuthentication) {
            return getServerInfo(logLevel, fastFail);
        }
    }

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
                 int stationConnectProtocolVersion,
                 int stationConnectFeatureFlags,
                 QString hostLayout,
                 QString virtualMode1,
                 QString virtualMode2,
                 QString captureSource,
                 QString& rtspSessionUrl,
                 QString& acceptedCaptureSource)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    QString stationConnectOutputArguments;
    if (m_StationConnectAuthentication && !captureDisplayMode.isEmpty()) {
        stationConnectOutputArguments =
                "&scProtocolVersion=" + QString::number(stationConnectProtocolVersion) +
                "&scFeatureFlags=" + QString::number(stationConnectFeatureFlags) +
                "&scDisplayMode=" + QString::fromLatin1(QUrl::toPercentEncoding(captureDisplayMode));
        stationConnectOutputArguments +=
                "&scCaptureSource=" +
                QString::fromLatin1(QUrl::toPercentEncoding(captureSource));
        if ((stationConnectFeatureFlags & NvOutputTopology::HostLayoutBindingFeature) != 0 &&
                !hostLayout.isEmpty()) {
            stationConnectOutputArguments +=
                    "&scHostLayout=" +
                    QString::fromLatin1(QUrl::toPercentEncoding(hostLayout));
            if ((stationConnectFeatureFlags &
                    NvOutputTopology::IndependentVirtualModesFeature) != 0) {
                if (!virtualMode1.isEmpty()) {
                    stationConnectOutputArguments +=
                            "&scVirtualMode1=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode1));
                }
                if (!virtualMode2.isEmpty()) {
                    stationConnectOutputArguments +=
                            "&scVirtualMode2=" +
                            QString::fromLatin1(QUrl::toPercentEncoding(virtualMode2));
                }
            }
        }
        if ((stationConnectFeatureFlags & NvOutputTopology::TopologyGenerationFeature) != 0 &&
                !topologyGeneration.isEmpty()) {
            stationConnectOutputArguments +=
                    "&scTopologyGeneration=" +
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
                                   "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                                   "&rikeyid="+QString::number(riKeyId)+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   stationConnectOutputArguments+
                                   LiGetLaunchUrlQueryParameters(),
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
    acceptedCaptureSource = getXmlString(response, "StationConnectCaptureSource");
    if (m_StationConnectAuthentication &&
            (acceptedCaptureSource.isEmpty() || acceptedCaptureSource != captureSource)) {
        throw GfeHttpResponseException(
                    400, "Host did not accept the requested capture source");
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

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    if (!m_StationConnectAuthentication) {
        return;
    }
    const QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
    if (!isStationConnectCertificate(certificate)) {
        const auto alternativeNames = certificate.subjectAlternativeNames();
        qWarning() << "Rejecting a TLS certificate outside the StationConnect profile"
                   << "null" << certificate.isNull()
                   << "selfSigned" << certificate.isSelfSigned()
                   << "keyAlgorithm" << certificate.publicKey().algorithm()
                   << "keyBits" << certificate.publicKey().length()
                   << "dnsSans" << alternativeNames.values(QSsl::DnsEntry).size()
                   << "ipSans" << alternativeNames.values(QSsl::IpAddressEntry).size()
                   << "effective" << certificate.effectiveDate()
                   << "expiry" << certificate.expiryDate();
        return;
    }
    for (const QSslError& error : errors) {
        switch (error.error()) {
        case QSslError::SelfSignedCertificate:
        case QSslError::CertificateUntrusted:
        case QSslError::UnableToGetLocalIssuerCertificate:
        case QSslError::UnableToVerifyFirstCertificate:
        case QSslError::HostNameMismatch:
            break;
        default:
            return;
        }
    }
    reply->ignoreSslErrors(errors);
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

QJsonObject NvHTTP::postStationConnectJson(QString command, const QJsonObject& body)
{
    if (!m_StationConnectAuthentication || !m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid StationConnect authentication state");
    }

    QUrl url(m_BaseUrlHttps);
    url.setPath("/stationconnect/auth/" + command);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setSslConfiguration(stationConnectSslConfiguration());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

    const auto sslErrorsConnection = connect(
        m_Nam, &QNetworkAccessManager::sslErrors,
        this, &NvHTTP::handleSslErrors);
    QNetworkReply* reply = m_Nam->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            &loop, &QEventLoop::quit);
    QTimer::singleShot(REQUEST_TIMEOUT_MS, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) {
        reply->abort();
    }
    m_Nam->clearAccessCache();
    disconnect(sslErrorsConnection);
    if (reply->error() != QNetworkReply::NoError) {
        const QString message = reply->errorString();
        delete reply;
        throw QtNetworkReplyException(QNetworkReply::UnknownNetworkError, message);
    }
    const QSslConfiguration negotiatedSsl = reply->sslConfiguration();
    if (!isStationConnectCertificate(negotiatedSsl.peerCertificate()) ||
            negotiatedSsl.sessionProtocol() != QSsl::TlsV1_3) {
        delete reply;
        throw GfeHttpResponseException(401,
                                       "StationConnect TLS validation failed");
    }
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    delete reply;
    if (!document.isObject()) {
        throw GfeHttpResponseException(400, "Malformed StationConnect authentication response");
    }
    return document.object();
}

QString NvHTTP::authenticate(QString username, QString password)
{
    SecureStringGuard passwordGuard(password);
    if (!m_StationConnectAuthentication || !m_SessionToken.isEmpty() ||
            username.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid StationConnect authentication state");
    }

    QJsonObject result = postStationConnectJson("start", {{"username", username}});
    for (int round = 0; round < 16; ++round) {
        const QString state = result.value("state").toString();
        if (state == "authenticated") {
            m_SessionToken = result.value("session_token").toString();
            if (m_SessionToken.isEmpty()) {
                throw GfeHttpResponseException(401, "Authentication returned no session token");
            }
            return m_SessionToken;
        }
        if (state == "denied") {
            throw GfeHttpResponseException(401, "Operating-system authentication failed");
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
        result = postStationConnectJson("respond", {
            {"conversation_id", result.value("conversation_id").toString()},
            {"responses", responses},
        });
    }

    throw GfeHttpResponseException(400, "PAM conversation exceeded the round limit");
}

NvOutputTopology NvHTTP::getOutputTopology()
{
    if (!m_StationConnectAuthentication || m_SessionToken.isEmpty()) {
        throw GfeHttpResponseException(400, "Invalid StationConnect topology state");
    }
    const QString response = openConnectionToString(
                m_BaseUrlHttps, "stationconnect/topology", nullptr,
                REQUEST_TIMEOUT_MS, NvLogLevel::NVLL_VERBOSE);
    const QJsonDocument document = QJsonDocument::fromJson(response.toUtf8());
    if (!document.isObject() && response.trimmed().startsWith(QLatin1Char('<'))) {
        // GameStream authorization failures use an XML status envelope even
        // for this StationConnect JSON endpoint. This is expected after a
        // display transition replaces the media worker and its in-memory
        // bearer sessions. Preserve the 401 so the bounded transition loop
        // can authenticate once to the replacement worker.
        verifyResponseStatus(response);
    }
    NvOutputTopology topology;
    QString error;
    if (!document.isObject() ||
            !NvOutputTopology::fromJson(document.object(), topology, &error)) {
        throw GfeHttpResponseException(400,
                                       error.isEmpty() ?
                                           "Malformed StationConnect topology response" : error);
    }
    return topology;
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Retain the protocol client identifier expected by the host.
    url.setQuery("uniqueid=0123456789ABCDEF&uuid=" +
                 QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request(url);

    if (baseUrl.scheme() == "https") {
        Q_ASSERT(m_StationConnectAuthentication);
        request.setSslConfiguration(stationConnectSslConfiguration());
        if (!m_SessionToken.isEmpty()) {
            request.setRawHeader("Authorization", "Bearer " + m_SessionToken.toUtf8());
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Use fine-grained idle timeouts to avoid calling QNetworkAccessManager::clearAccessCache(),
    // which tears down the NAM's global thread each time. We must not keep persistent connections
    // or GFE will puke.
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    auto sslErrorsConnection = connect(m_Nam, &QNetworkAccessManager::sslErrors, this, &NvHTTP::handleSslErrors);
    QNetworkReply* reply = m_Nam->get(request);

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

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

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            GfeHttpResponseException exception(401, "StationConnect TLS validation failed");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    const bool stationConnectTls = baseUrl.scheme() == "https";
    const bool approvedCertificate = !stationConnectTls ||
            isStationConnectCertificate(reply->sslConfiguration().peerCertificate());
    const bool approvedProtocol = !stationConnectTls ||
            reply->sslConfiguration().sessionProtocol() == QSsl::TlsV1_3;
    if (!approvedCertificate || !approvedProtocol) {
        qWarning() << "Rejecting StationConnect TLS session"
                   << "certificate" << approvedCertificate
                   << "tls13" << approvedProtocol;
        GfeHttpResponseException exception(401, "Invalid StationConnect TLS session");
        delete reply;
        throw exception;
    }

    return reply;
}

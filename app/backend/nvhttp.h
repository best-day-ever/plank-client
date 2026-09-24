#pragma once

#include "nvapp.h"
#include "nvaddress.h"
#include "outputtopology.h"
#include "desktopstage.h"
#include "macpreviewlaunch.h"
#include "hosttlsguard.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <functional>

class NvComputer;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class GfeHttpResponseException : public std::exception
{
public:
    GfeHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.constData();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.constData();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_StatusMessage) + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QByteArray m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText) :
        m_Error(error),
        m_ErrorText(errorText.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.constData();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.constData();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_ErrorText) + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QByteArray m_ErrorText;
};

class MacSessionActiveException : public GfeHttpResponseException
{
public:
    explicit MacSessionActiveException(const QString& sessionId) :
        GfeHttpResponseException(409, "PLANK workstation session is active"),
        m_SessionId(sessionId) {}
    const QString& sessionId() const { return m_SessionId; }
private:
    QString m_SessionId;
};

class HostIdentityChangedException : public QtNetworkReplyException
{
public:
    HostIdentityChangedException(QString endpoint, QByteArray previous, QByteArray replacement) :
        QtNetworkReplyException(QNetworkReply::SslHandshakeFailedError,
            "Host identity changed. Connect again to review the replacement before signing in."),
        endpoint(std::move(endpoint)), previousKey(std::move(previous)), replacementKey(std::move(replacement)) {}
    const QString endpoint;
    const QByteArray previousKey, replacementKey;
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(NvComputer* computer, QNetworkAccessManager* nam = nullptr);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel = NvLogLevel::NVLL_VERBOSE);

    void setAddress(NvAddress address);

    void setPlankSessionToken(QString sessionToken, QByteArray identityKey);
    void setTrustAddress(NvAddress address);
    QByteArray hostIdentityKey() const { return m_IdentityKey; }
    void setTrustPrompt(std::function<bool(const HostIdentityChangedException&)> prompt) {
        m_TrustPrompt = std::move(prompt);
    }

    // Brokered (remote) mode: every HTTPS connection to this host must present
    // exactly this leaf (SHA-256 over DER, lower-case hex), replacing the
    // profile-only acceptance. The RSA-3072/TLS 1.3 profile check still applies.
    void setPinnedCertificateSha256(QString certificateSha256);
    bool isBrokered() const { return !m_PinnedCertificateSha256.isEmpty(); }
    QString pinnedCertificateSha256() const { return m_PinnedCertificateSha256; }

    // Used only by the session recovery worker; ordinary discovery/login has
    // no gate. False cancels, while the callback may wait for a local decision.
    void setRequestGate(std::function<bool(bool)> gate) { m_RequestGate = std::move(gate); }

    enum class AuthenticationIntent { ExplicitConnection, Recovery };
    QString authenticate(QString username, QString password, bool* greeterConfirmed = nullptr,
                         AuthenticationIntent intent = AuthenticationIntent::ExplicitConnection);
    // Brokered admission: /plank/auth/start with a one-use GSSAPI token minted
    // by the broker. Never answers a password challenge.
    QString authenticateGssapi(QString username, QString gssapiToken, bool* greeterConfirmed = nullptr);
    bool probeWorkerReplacement(const QString& instance, const QString& certificateSha256);
    QString workerInstance() const { return m_WorkerInstance; }
    // Owner and sign-out offer from the last refused launch or resume.
    PlankDesktopSignOut desktopSignOut() const { return m_DesktopSignOut; }
    // PlankDisplayArrangementError of the last refused launch or resume
    // (400 or 409 with the display arrangement extension), else empty.
    QString displayArrangementError() const { return m_DisplayArrangementError; }
    NvOutputTopology getOutputTopology(QString* certificateSha256 = nullptr);
    NvOutputTopology prepareMacDisplay(const QString& mode, const QString& encodingMode, int scale = 1,
                                      const QString& takeoverSessionId = QString());
    MacPreviewLaunch::Reply startMacPreview(const NvOutputTopology& topology,
                                           const QString& certificateSha256,
                                           int bitrateKbps, int udpPayloadSize);

    NvAddress address();

    uint16_t controlPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    startApp(QString verb,
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
             int primaryOutput = -1);

    QVector<NvApp>
    getAppList();

    QImage
    getBoxArt(int appId);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    QUrl m_BaseUrlHttps;
private:
    void waitForRequestPermission(bool authenticating = false);
    void establishHostTrust(AuthenticationIntent intent);
    void checkTlsGuard(const HostTlsGuard& guard);

    QNetworkReply*
    openConnection(QUrl baseUrl,
                   QString command,
                   QString arguments,
                   int timeoutMs,
                   NvLogLevel logLevel, HostTlsGuard::Mode trustMode = HostTlsGuard::Mode::Observe);

    QJsonObject postPlankJson(QString command, const QJsonObject& body);
    QJsonObject postPinnedMacJson(const QString& path, const QJsonObject& body,
                                 const QString& certificateSha256);

    NvAddress m_Address;
    bool acceptsPlankCertificate(const QSslCertificate& certificate) const;
    QMetaObject::Connection enforcePinnedCertificate(QNetworkAccessManager* manager);

    QNetworkAccessManager* m_Nam;
    QString m_PinnedCertificateSha256;
    QString m_SessionToken;
    QString m_WorkerInstance;
    PlankDesktopSignOut m_DesktopSignOut;
    QString m_DisplayArrangementError;
    HostTrustStore m_TrustStore;
    QString m_TrustEndpoint;
    QByteArray m_IdentityKey;
    std::function<bool(const HostIdentityChangedException&)> m_TrustPrompt;
    std::function<bool(bool)> m_RequestGate;
};

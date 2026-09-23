#include "plankbrokerclient.h"
#include "plankenrollment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QScopedPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>

namespace {
constexpr int RequestTimeoutMs = 10000;
// Password + OTP verification goes through FAST-armoured Kerberos on the
// broker; allow the KDC round trips some slack.
constexpr int AuthenticationTimeoutMs = 20000;
// Enrolment steps add IPA directory writes and, for the last code, a full
// password + code login on top of the Kerberos round trips.
constexpr int EnrollmentTimeoutMs = 30000;

bool leafMatchesPins(const QSslConfiguration& configuration, const QStringList& pins)
{
    const QSslCertificate leaf = configuration.peerCertificate();
    return !leaf.isNull() && PlankBroker::spkiPinMatches(leaf.toDer(), pins);
}
}

QString PlankBrokerError::userMessage() const
{
    switch (m_Kind) {
    case NotConfigured:
        return QCoreApplication::translate("PlankBroker",
            "Remote access is not configured. Add the broker key (SPKI SHA-256 pin) in Settings.");
    case Network:
        return QCoreApplication::translate("PlankBroker",
            "The remote access server could not be reached.");
    case Tls:
        return QCoreApplication::translate("PlankBroker",
            "The remote access server's identity could not be verified.");
    case Denied:
        return QCoreApplication::translate("PlankBroker", "Sign-in failed.");
    case RateLimited:
        return QCoreApplication::translate("PlankBroker",
            "Too many attempts. Try again in %1 seconds.").arg(qMax(1, m_RetryAfter));
    case SessionExpired:
        return QCoreApplication::translate("PlankBroker",
            "Your remote session has ended. Please sign in again.");
    case Unavailable:
        return QCoreApplication::translate("PlankBroker",
            "The workstation is temporarily unavailable.");
    case Protocol:
    default:
        return QCoreApplication::translate("PlankBroker",
            "The remote access server returned an unexpected response.");
    }
}

PlankBrokerClient::PlankBrokerClient(Config config)
    : m_Config(std::move(config))
{
    m_Config.host = m_Config.host.trimmed();
    m_Config.pins = PlankBroker::normalizePins(m_Config.pins);
}

void PlankBrokerClient::checkConfigured() const
{
    if (m_Config.pins.isEmpty() || m_Config.port == 0 ||
            !PlankBroker::isEndpointName(m_Config.host)) {
        throw PlankBrokerError(PlankBrokerError::NotConfigured);
    }
}

PlankBrokerClient::Response PlankBrokerClient::request(const QByteArray& method, const QString& path,
                                                       const QJsonObject* body,
                                                       const QString& sessionToken) const
{
    checkConfigured();
    if (!sessionToken.isEmpty() &&
            !PlankBroker::isPrintableAscii(sessionToken, PlankBroker::MaximumTokenLength)) {
        throw PlankBrokerError(PlankBrokerError::SessionExpired);
    }

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(m_Config.host);
    url.setPort(m_Config.port);
    url.setPath(path, QUrl::StrictMode);

    QNetworkRequest request(url);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_3OrLater);
    // No WebPKI: with no trust anchors every handshake reports errors, and
    // the only way through is the SPKI pin check below.
    ssl.setCaCertificates({});
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(ssl);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    QByteArray payload;
    if (body != nullptr) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        payload = QJsonDocument(*body).toJson(QJsonDocument::Compact);
    }
    if (!sessionToken.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + sessionToken.toLatin1());
        if (m_Config.deviceSigner) {
            // Section 14.2: prove possession of the device key the session is
            // bound to. No key on this device: unbound, send as before. Signing
            // impossible right now (locked, helper error): do not send an
            // unsigned request that a bound session would answer with 401.
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            const QByteArray message = PlankBroker::deviceProofMessage(
                        method, url.path(QUrl::FullyEncoded).toUtf8(), now, sessionToken, payload);
            const DeviceSignature signature = m_Config.deviceSigner(message);
            if (signature.status == DeviceSignature::Signed && PlankBroker::isDeviceSignature(signature.signature)) {
                request.setRawHeader(PlankBroker::deviceTimeHeader(), QByteArray::number(now));
                request.setRawHeader(PlankBroker::deviceProofHeader(), signature.signature.toLatin1());
            } else if (signature.status == DeviceSignature::NoKey) {
                qWarning() << "Remote access: no device key was found for this bearer request";
            } else {
                qWarning() << "Remote access: the device key could not sign this request; not sending it";
                throw PlankBrokerError(PlankBrokerError::Network);
            }
        }
    }

    // A fresh manager per request guarantees the encrypted() signal (the pin
    // is re-checked there before any request bytes are written).
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    bool verified = false;
    bool rejected = false;
    const QStringList pins = m_Config.pins;
    QObject::connect(&manager, &QNetworkAccessManager::sslErrors, &manager,
                     [&](QNetworkReply* reply, const QList<QSslError>& errors) {
        if (leafMatchesPins(reply->sslConfiguration(), pins)) {
            reply->ignoreSslErrors(errors);
        } else {
            rejected = true;
        }
    });
    QObject::connect(&manager, &QNetworkAccessManager::encrypted, &manager, [&](QNetworkReply* reply) {
        const QSslConfiguration negotiated = reply->sslConfiguration();
        if (leafMatchesPins(negotiated, pins) && negotiated.sessionProtocol() == QSsl::TlsV1_3) {
            verified = true;
        } else {
            rejected = true;
            reply->abort();
        }
    });

    QNetworkReply* rawReply = body != nullptr ?
                manager.sendCustomRequest(request, method, payload) :
                manager.sendCustomRequest(request, method);
    QScopedPointer<QNetworkReply> reply(rawReply);
    reply->setReadBufferSize(PlankBroker::MaximumReplyBytes + 1);
    QByteArray response;
    bool oversized = false;
    auto drain = [&]() {
        response += reply->read(PlankBroker::MaximumReplyBytes + 1 - response.size());
        if (response.size() > PlankBroker::MaximumReplyBytes) {
            oversized = true;
            reply->abort();
        }
    };
    QEventLoop loop;
    QObject::connect(reply.data(), &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (QCoreApplication::instance() != nullptr) {
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                         &loop, &QEventLoop::quit);
    }
    const bool authentication = path.startsWith(QLatin1String("/v1/auth/"));
    const bool enrollment = path.startsWith(QLatin1String("/v1/enroll/")) ||
                            path == QLatin1String("/v1/passkeys/setup");
    QTimer::singleShot(enrollment ? EnrollmentTimeoutMs :
                       authentication ? AuthenticationTimeoutMs : RequestTimeoutMs,
                       &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) reply->abort();
    if (!oversized) drain();

    // Nothing from an unverified peer is interpreted, not even a status.
    if (rejected) {
        response.fill('\0');
        throw PlankBrokerError(PlankBrokerError::Tls);
    }
    if (!verified) {
        response.fill('\0');
        throw PlankBrokerError(reply->error() == QNetworkReply::SslHandshakeFailedError ?
                                   PlankBrokerError::Tls : PlankBrokerError::Network);
    }
    if (oversized) {
        response.fill('\0');
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    Response result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (result.status == 0) {
        throw PlankBrokerError(PlankBrokerError::Network);
    }
    result.body = response;
    return result;
}

PlankBroker::AuthReply PlankBrokerClient::start(const QString& username, PlankBroker::AuthMethod method) const
{
    if (username.isEmpty() || username.size() > 255) {
        throw PlankBrokerError(PlankBrokerError::Denied);
    }
    QJsonObject body {{QStringLiteral("username"), username}};
    if (method == PlankBroker::AuthMethod::Passkey) {
        body.insert(QStringLiteral("method"), QStringLiteral("passkey"));
    }
    if (m_Config.devicePublicKey) {
        // Section 14.2: bind the session to this device when a key is at
        // hand; otherwise sign in unbound (never fail because of the key).
        const QString deviceKey = m_Config.devicePublicKey();
        if (PlankBroker::isDevicePublicKey(deviceKey)) {
            body.insert(QStringLiteral("device_key"), deviceKey);
        }
    }
    Response response = request("POST", QStringLiteral("/v1/auth/start"), &body, QString());
    const PlankBroker::AuthReply reply = PlankBroker::parseAuthReply(response.status, response.body);
    response.body.fill('\0');
    switch (reply.kind) {
    case PlankBroker::ReplyKind::Challenge:
        return reply;
    case PlankBroker::ReplyKind::Denied:
        throw PlankBrokerError(PlankBrokerError::Denied);
    case PlankBroker::ReplyKind::RateLimited:
        throw PlankBrokerError(PlankBrokerError::RateLimited, reply.retryAfter);
    default:
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
}

PlankBroker::AuthReply PlankBrokerClient::respond(const QString& conversationId,
                                                  const QJsonArray& responses) const
{
    const QJsonObject body {
        {QStringLiteral("conversation_id"), conversationId},
        {QStringLiteral("responses"), responses},
    };
    return sendRespond(body);
}

PlankBroker::AuthReply PlankBrokerClient::respondPasskey(const QString& conversationId,
                                                         const PlankBroker::PasskeyAssertion& assertion) const
{
    const QJsonObject body {
        {QStringLiteral("conversation_id"), conversationId},
        {QStringLiteral("passkey"), QJsonObject {
             {QStringLiteral("credential_id"), assertion.credentialId},
             {QStringLiteral("authenticator_data"), assertion.authenticatorData},
             {QStringLiteral("signature"), assertion.signature},
         }},
    };
    return sendRespond(body);
}

PlankBroker::AuthReply PlankBrokerClient::sendRespond(const QJsonObject& body) const
{
    Response response = request("POST", QStringLiteral("/v1/auth/respond"), &body, QString());
    const PlankBroker::AuthReply reply = PlankBroker::parseAuthReply(response.status, response.body);
    response.body.fill('\0');
    switch (reply.kind) {
    case PlankBroker::ReplyKind::Challenge:
    case PlankBroker::ReplyKind::Authenticated:
        return reply;
    case PlankBroker::ReplyKind::Denied:
        throw PlankBrokerError(PlankBrokerError::Denied);
    case PlankBroker::ReplyKind::RateLimited:
        throw PlankBrokerError(PlankBrokerError::RateLimited, reply.retryAfter);
    default:
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
}

PlankBrokerClient::PasskeySignIn PlankBrokerClient::signInWithPasskey(const QString& username, const QString& rpId,
                                                                      const PasskeyAssertor& assertor) const
{
    PasskeySignIn outcome;
    PlankBroker::AuthReply challenge;
    try {
        challenge = start(username, PlankBroker::AuthMethod::Passkey);
    } catch (const PlankBrokerError& error) {
        if (error.kind() != PlankBrokerError::Denied) throw;
        return outcome;
    }
    const PlankBroker::Prompt* prompt = PlankBroker::passkeyPrompt(challenge.prompts);
    // The key on this Mac belongs to the configured relying party only.
    if (prompt == nullptr || prompt->passkey.rpId != rpId) return outcome;

    PlankBroker::PasskeyAssertion assertion;
    switch (assertor(prompt->passkey, assertion)) {
    case PasskeyAssertResult::Signed:
        break;
    case PasskeyAssertResult::NotConfirmed:
        outcome.result = PasskeySignIn::NotConfirmed;
        return outcome;
    case PasskeyAssertResult::NoMatchingKey:
    case PasskeyAssertResult::Failed:
    default:
        return outcome;
    }
    try {
        PlankBroker::AuthReply reply = respondPasskey(challenge.conversationId, assertion);
        if (reply.kind == PlankBroker::ReplyKind::Authenticated) {
            outcome.result = PasskeySignIn::Authenticated;
            outcome.reply = reply;
        }
    } catch (const PlankBrokerError& error) {
        if (error.kind() != PlankBrokerError::Denied) throw;
    }
    return outcome;
}

PlankBrokerClient::Response PlankBrokerClient::post(const QString& path, const QJsonObject& body) const
{
    return request("POST", path, &body, QString());
}

PlankBrokerClient::Response PlankBrokerClient::get(const QString& path) const
{
    return request("GET", path, nullptr, QString());
}

void PlankBrokerClient::download(const QString& path, const QString& destination,
                                 qint64 expectedSize, const QByteArray& expectedSha256) const
{
    checkConfigured();
    if (!path.startsWith(QStringLiteral("/v1/client-updates/macos-arm64/")) ||
            path.contains(QStringLiteral("..")) || expectedSize <= 0 ||
            expectedSize > 2LL * 1024 * 1024 * 1024 || expectedSha256.size() != 64) {
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(m_Config.host);
    url.setPort(m_Config.port);
    url.setPath(path, QUrl::StrictMode);
    QNetworkRequest request(url);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_3OrLater);
    ssl.setCaCertificates({});
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(ssl);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept", "application/x-apple-diskimage");

    QFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        throw PlankBrokerError(PlankBrokerError::Network);
    }
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    bool verified = false;
    bool rejected = false;
    const QStringList pins = m_Config.pins;
    QObject::connect(&manager, &QNetworkAccessManager::sslErrors, &manager,
                     [&](QNetworkReply* reply, const QList<QSslError>& errors) {
        if (leafMatchesPins(reply->sslConfiguration(), pins)) reply->ignoreSslErrors(errors);
        else rejected = true;
    });
    QObject::connect(&manager, &QNetworkAccessManager::encrypted, &manager, [&](QNetworkReply* reply) {
        const auto negotiated = reply->sslConfiguration();
        if (leafMatchesPins(negotiated, pins) && negotiated.sessionProtocol() == QSsl::TlsV1_3) {
            verified = true;
        } else {
            rejected = true;
            reply->abort();
        }
    });
    QScopedPointer<QNetworkReply> reply(manager.get(request));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 received = 0;
    bool invalid = false;
    auto drain = [&]() {
        const QByteArray chunk = reply->readAll();
        received += chunk.size();
        if (received > expectedSize || output.write(chunk) != chunk.size()) {
            invalid = true;
            reply->abort();
        } else {
            hash.addData(chunk);
        }
    };
    QEventLoop loop;
    QObject::connect(reply.data(), &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10 * 60 * 1000, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!reply->isFinished()) reply->abort();
    if (!invalid) drain();
    output.close();
    const bool ok = verified && !rejected && !invalid &&
                    reply->error() == QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                    received == expectedSize && hash.result().toHex() == expectedSha256;
    if (!ok) {
        QFile::remove(destination);
        throw PlankBrokerError(rejected ? PlankBrokerError::Tls :
                               invalid || (verified && reply->error() == QNetworkReply::NoError) ?
                                   PlankBrokerError::Protocol : PlankBrokerError::Network);
    }
}

void PlankBrokerClient::throwForBearerStatus(int status, const QByteArray& body)
{
    switch (PlankBroker::classifyBearerStatus(status)) {
    case PlankBroker::BearerStatus::Ok:
        return;
    case PlankBroker::BearerStatus::SessionExpired:
        throw PlankBrokerError(PlankBrokerError::SessionExpired);
    case PlankBroker::BearerStatus::RateLimited: {
        QJsonObject object;
        const int retryAfter = PlankBroker::parseObject(body, object) ?
                    PlankBroker::retryAfterFrom(object) : PlankBroker::DefaultRetryAfterSeconds;
        throw PlankBrokerError(PlankBrokerError::RateLimited, retryAfter);
    }
    case PlankBroker::BearerStatus::Unavailable: {
        if (PlankBroker::isTransientOfflineResponse(status, body)) {
            throw PlankBrokerError(PlankBrokerError::Unavailable);
        }
        throw PlankBrokerError(PlankBrokerError::Denied);
    }
    case PlankBroker::BearerStatus::Denied:
        throw PlankBrokerError(PlankBrokerError::Denied);
    default:
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
}

QVector<PlankBroker::Host> PlankBrokerClient::hosts(const QString& sessionToken) const
{
    if (sessionToken.isEmpty()) throw PlankBrokerError(PlankBrokerError::SessionExpired);
    const Response response = request("GET", QStringLiteral("/v1/hosts"), nullptr, sessionToken);
    throwForBearerStatus(response.status, response.body);
    QVector<PlankBroker::Host> hosts;
    if (!PlankBroker::parseHosts(response.body, hosts)) {
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    return hosts;
}

PlankBroker::Lease PlankBrokerClient::connect(const QString& sessionToken, const QString& hostId) const
{
    if (sessionToken.isEmpty()) {
        qWarning() << "Remote access: broker session token is absent before connect";
        throw PlankBrokerError(PlankBrokerError::SessionExpired);
    }
    if (!PlankBroker::isHostId(hostId)) throw PlankBrokerError(PlankBrokerError::Denied);
    const QJsonObject empty;
    Response response = request("POST", PlankBroker::hostActionPath(hostId, QStringLiteral("connect")),
                                &empty, sessionToken);
    throwForBearerStatus(response.status, response.body);
    PlankBroker::Lease lease;
    const bool parsed = PlankBroker::parseLease(response.body, lease);
    response.body.fill('\0');
    if (!parsed) throw PlankBrokerError(PlankBrokerError::Protocol);
    return lease;
}

void PlankBrokerClient::keepalive(const QString& sessionToken, const QString& hostId) const
{
    if (sessionToken.isEmpty()) throw PlankBrokerError(PlankBrokerError::SessionExpired);
    if (!PlankBroker::isHostId(hostId)) throw PlankBrokerError(PlankBrokerError::Denied);
    const QJsonObject empty;
    const Response response = request("POST", PlankBroker::hostActionPath(hostId, QStringLiteral("keepalive")),
                                      &empty, sessionToken);
    throwForBearerStatus(response.status, response.body);
}

void PlankBrokerClient::logout(const QString& sessionToken) const
{
    if (sessionToken.isEmpty()) return;
    const QJsonObject empty;
    const Response response = request("POST", QStringLiteral("/v1/logout"), &empty, sessionToken);
    throwForBearerStatus(response.status, response.body);
}

void PlankBrokerClient::setupPasskey(const QString& sessionToken, const QString& mapping) const
{
    if (sessionToken.isEmpty()) throw PlankBrokerError(PlankBrokerError::SessionExpired);
    if (!PlankEnrollment::isPasskeyMapping(mapping)) throw PlankBrokerError(PlankBrokerError::Protocol);
    const QJsonObject body {{QStringLiteral("mapping"), mapping}};
    Response response = request("POST", QStringLiteral("/v1/passkeys/setup"), &body, sessionToken);
    throwForBearerStatus(response.status, response.body);
    QJsonObject result;
    const bool added = PlankBroker::parseObject(response.body, result) &&
            result.value(QStringLiteral("state")) == QLatin1String("passkey_added");
    response.body.fill('\0');
    if (!added) throw PlankBrokerError(PlankBrokerError::Protocol);
}

void PlankBrokerClient::skipPasskeySetup(const QString& sessionToken) const
{
    if (sessionToken.isEmpty()) return;
    Response response = request("POST", QStringLiteral("/v1/passkeys/setup/skip"), nullptr, sessionToken);
    throwForBearerStatus(response.status, response.body);
    response.body.fill('\0');
}

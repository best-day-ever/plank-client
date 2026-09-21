#include <QtTest>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>

#include "plankbroker.h"
#include "plankbrokerclient.h"
#include "macpreviewlaunch.h"
#include "outputtopology.h"

// Remote (broker) mode: bde-linux docs/plank-broker.md sections 10.1 / 10.2.
// Certificate fixtures were generated with OpenSSL; their expected pins were
// computed independently with
//   openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | sha256
// (SPKI) and `openssl dgst -sha256` over the DER (leaf).

namespace {

const char EcCertificateBase64[] =
    "MIIBtDCCAVmgAwIBAgIUCRdNSSSN3QeyedMR/2NNJjI1CRowCgYIKoZIzj0EAwIwHjEcMBoGA1UE"
    "AwwTYnJva2VyLmV4YW1wbGUudGVzdDAgFw0yNjA5MjExNDA5NTRaGA8yMTI2MDgyODE0MDk1NFow"
    "HjEcMBoGA1UEAwwTYnJva2VyLmV4YW1wbGUudGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IA"
    "BILYjWo0puuGeIwCH04JTdCwlrSOeTNC3g5S7fRp9AEGiT1gtZ8dGcrGOifhmaMrgnQA8NBiABzr"
    "8VlkuGwcOrajczBxMB0GA1UdDgQWBBTOHP6zpycxw0Iuu9Qb8VWEN3lVjzAfBgNVHSMEGDAWgBTO"
    "HP6zpycxw0Iuu9Qb8VWEN3lVjzAPBgNVHRMBAf8EBTADAQH/MB4GA1UdEQQXMBWCE2Jyb2tlci5l"
    "eGFtcGxlLnRlc3QwCgYIKoZIzj0EAwIDSQAwRgIhAKJtlaaULDAPMm3HniayaWPdtlOPUXKwWvAU"
    "US8KGpB6AiEAnekyGhNtj05Lfadv9uVX8C8UOhoH5H/ztN2hNc+ui0g=";
const char EcSpkiSha256[] = "91bb7956f2378b1ed82e7f52b18d5ce8ca2a4f6d835919872083498f517b09c8";

const char RsaCertificateBase64[] =
    "MIIEOTCCAqGgAwIBAgIUUkxOM3CFJXvrZWgwWytYsPpi4XQwDQYJKoZIhvcNAQELBQAwHDEaMBgG"
    "A1UEAwwRaG9zdC5leGFtcGxlLnRlc3QwIBcNMjYwOTIxMTQwOTU0WhgPMjEyNjA4MjgxNDA5NTRa"
    "MBwxGjAYBgNVBAMMEWhvc3QuZXhhbXBsZS50ZXN0MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIB"
    "igKCAYEAv+v5pjnodgUGMi5xZmPTdhVBJBJMPFft/FwzNLVP/hSnnp5LO9m0Sjj9i40mB4NX9aV2"
    "iCOKp8KXpup6V2zy7kwCHOVx4EM/VPhAP3P43VUYr7ABq17at6hsqR5vII3T+7dSfvRlyHIiOOLd"
    "lnD01cSxckIiWye7TOhXh/LJWYEGItZom6Lnlo35iNmUIKtnEq89nhYGhzSE95cNIZqUqNJNf6Sz"
    "gH4od7r7lg2Y4qi2oBVxU+8m/kpGWH31bWM89FncvLai7NAYLmr7zkkv+/C9zfozcPg5FtWDBMls"
    "3JpT9DnYjw31AQa3CIW7SxiuV08K7p8Jjp7hwrWnq3Tdxu73FgqeGrHBgCZsLMEr3C/+YsokoiId"
    "+l1Sgmk4xEU+2eDAEOCrDJwGKeFjFm2S5AmTQRRscSD9YgUR3ZY7oceSW8Sw6aemCSsvX/XVSwas"
    "Hh6dMneFNrGeWzN5QxcJfeQRpnYvN0SktgRAhpbzjapyHZeDu2eVtcnDA7+50q5rAgMBAAGjcTBv"
    "MB0GA1UdDgQWBBQw5fFf/tEKi9v3JdNmQZBV6dFTMjAfBgNVHSMEGDAWgBQw5fFf/tEKi9v3JdNm"
    "QZBV6dFTMjAPBgNVHRMBAf8EBTADAQH/MBwGA1UdEQQVMBOCEWhvc3QuZXhhbXBsZS50ZXN0MA0G"
    "CSqGSIb3DQEBCwUAA4IBgQC+tdTXrqV4M8fNbpydXacqUComCvB/44facaHmT0VTaKMtXAQ1/+Ox"
    "eDxG/PFOl+Ui0JJw6Z9mCphxAlUv6Tyv8Q1+V/U1HqbT/gLk07xhw43BZ/2eT1whsKwjsAPhPNOK"
    "BWWLJWRYTXtrxlRQ8iuZkIu90JDDFoYOuOk91cXncKJQ+jcicbPfdZIzD7bD1CrgQtiFIO7exvDm"
    "MdrZDjazQeiOcZ7QmEn5XmPpNMliDl2soj/+wl7/Enz+I+2P3BViThFt8qHjLNrde927jvIZHCib"
    "VbkFC+zF47NJjRmhsfdq5QH+sGRG7mkWSDytgczz+EmC5mqQr6jBCU84Nn7gGqgXCV4+uxXGG4EZ"
    "Z7KQZQGHye9K732NsS01N0CtHyqc0Cw+jIBoYIJDrHfTOch4PpWplq3ppVk0fJkIZwG0b2Qf2VxK"
    "UypLM8xOYdqm0V+dnJvzmK1PsA3ZMMEuO8gwIBy+j9+j5r1tEy4uBj7L+eIQZvR1gy/jaNA5T+vc"
    "NOw=";
const char RsaSpkiSha256[] = "6d1f9a85f1550a31f9a8c785d990dd2099d278e3ff36ee43f956fc37c39b769e";
const char RsaLeafSha256[] = "11ac435667fdae3f2abbec034497f69b8e54c894a3336975ba1ba20cb883de6e";

// Throwaway key for the in-process TLS tests below only; it matches the
// EC certificate above and protects nothing.
const char EcTestOnlyPrivateKeyPem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgczd2sUy0hEU5M5o/\n"
    "XskQT9pkYDROwzubJkO4EjHBmE+hRANCAASC2I1qNKbrhniMAh9OCU3QsJa0jnkz\n"
    "Qt4OUu30afQBBok9YLWfHRnKxjon4ZmjK4J0APDQYgAc6/FZZLhsHDq2\n"
    "-----END PRIVATE KEY-----\n";

QByteArray ecCertificate() { return QByteArray::fromBase64(EcCertificateBase64); }
QByteArray rsaCertificate() { return QByteArray::fromBase64(RsaCertificateBase64); }
QByteArray json(const char* text) { return QByteArray(text); }

// Minimal HTTPS/1.1 responder: one canned reply per connection, records the
// raw request bytes it received (to prove nothing is sent to a wrong peer).
class TestBrokerServer : public QObject
{
public:
    explicit TestBrokerServer(QSsl::SslProtocol protocol)
    {
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(QSslCertificate(ecCertificate(), QSsl::Der));
        configuration.setPrivateKey(QSslKey(QByteArray(EcTestOnlyPrivateKeyPem), QSsl::Ec, QSsl::Pem));
        configuration.setProtocol(protocol);
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_Server.setSslConfiguration(configuration);
        QObject::connect(&m_Server, &QSslServer::pendingConnectionAvailable, this, [this]() {
            while (QTcpSocket* socket = m_Server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    request += socket->readAll();
                    const int headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int contentLength = 0;
                    for (const QByteArray& line : request.left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (request.size() < headerEnd + 4 + contentLength) return;
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_Server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_Server.serverPort(); }

    QByteArray status = "200 OK";
    QByteArray body;
    QByteArray request;

private:
    QSslServer m_Server;
};

PlankBrokerClient::Config localConfig(quint16 port, const QStringList& pins)
{
    PlankBrokerClient::Config config;
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.pins = pins;
    return config;
}

const QString HostPin = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

QJsonObject fixedCaptureTopology()
{
    return QJsonDocument::fromJson(R"({
      "schema_version": 13, "feature_flags": 3670129,
      "generation": "98454815-80ab-4a88-b187-92f59353afca",
      "capture": {"id": "cgdisplay:42", "width": 3840, "height": 2160,
        "logical_bounds": {"x": -1920, "y": 0, "width": 1920, "height": 1080},
        "encoding_profile": {"capture_source": "screencapturekit", "encoder_backend": "videotoolbox",
          "encoding_mode": "hevc-10-420-videotoolbox", "codec": "hevc", "profile": "main10",
          "bit_depth": 10, "chroma": "4:2:0", "range": "full", "matrix": "bt709",
          "primaries": "bt709", "transfer": "srgb", "rgb_identity": false}}})").object();
}

}

class TestPlankBroker : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // Pins and certificates
    void normalizesPins();
    void parsesPinLists();
    void shipsCanonicalDefaultPins();
    void extractsSpkiFromEcAndRsaCertificates();
    void spkiPinMatches();
    void spkiPinMismatchIsRejected();
    void spkiMatchesAnyOfMultiplePins();
    void emptyOrInvalidPinsNeverMatch();
    void malformedCertificatesAreRejected();
    void hostLeafPin();

    // Broker replies
    void parsesChallenge();
    void parsesAuthenticated();
    void parsesDenied();
    void parsesRateLimit();
    void rejectsMalformedAuthReplies();
    void answersPrompts();
    void parsesHosts();
    void rejectsMalformedHosts();
    void parsesLease();
    void rejectsMalformedLeases();
    void parsesLeaseRoute();
    void classifiesBearerStatus();
    void encodesHostActionPath();
    void parsesHostAdmission();
    void clientRefusesWithoutPins();
    void errorMessagesAreGeneric();

    // Brokered connect overrides
    void brokeredTransportUsesLeasedPort();
    void brokeredTransportRequiresPinnedCertificate();
    void directTransportIsUnchanged();
    void brokeredMacLaunchIgnoresUdpPort();

    // Real TLS through the active Qt TLS backend
    void tlsPinnedRoundTrip();
    void tlsWrongPinSendsNothing();
    void tlsRateLimitOnBearerCall();
    void tlsRequiresTls13();
    void tlsConnectAsksForRelayOnlyWhenForced();

    // Keepalive
    void keepaliveCadence();
    void keepaliveRetriesThenGivesUp();
};

void TestPlankBroker::initTestCase()
{
#ifdef Q_OS_MACOS
    // Mirror main.cpp: the broker requires TLS 1.3, which Qt's default
    // SecureTransport backend cannot negotiate. Select the bundled OpenSSL
    // backend exactly as the Client does rather than weakening the policy.
    QVERIFY2(QSslSocket::setActiveBackend(QStringLiteral("openssl")),
             qPrintable(QSslSocket::availableBackends().join(u", ")));
    QVERIFY(QSslSocket::supportedProtocols().contains(QSsl::TlsV1_3));
#endif
}

void TestPlankBroker::normalizesPins()
{
    const QString canonical = QString::fromLatin1(EcSpkiSha256);
    QCOMPARE(PlankBroker::normalizePin(canonical), canonical);
    QCOMPARE(PlankBroker::normalizePin(canonical.toUpper()), canonical);
    QString colons;
    for (int i = 0; i < canonical.size(); i += 2) {
        if (i) colons += QLatin1Char(':');
        colons += canonical.mid(i, 2).toUpper();
    }
    QCOMPARE(PlankBroker::normalizePin(colons), canonical);
    QCOMPARE(PlankBroker::normalizePin(QStringLiteral("  ") + canonical + QStringLiteral(" ")), canonical);
    QVERIFY(PlankBroker::normalizePin(canonical.left(63)).isEmpty());
    QVERIFY(PlankBroker::normalizePin(canonical + QStringLiteral("0")).isEmpty());
    QVERIFY(PlankBroker::normalizePin(QString(canonical).replace(0, 1, QStringLiteral("g"))).isEmpty());
    QVERIFY(!PlankBroker::isCanonicalSha256Hex(canonical.toUpper()));
}

void TestPlankBroker::parsesPinLists()
{
    const QString ec = QString::fromLatin1(EcSpkiSha256);
    const QString rsa = QString::fromLatin1(RsaSpkiSha256);
    QStringList rejected;
    const QStringList pins = PlankBroker::parsePinList(
                ec.toUpper() + QStringLiteral("\n\n  ") + rsa + QStringLiteral(", not-a-pin;") + ec,
                &rejected);
    QCOMPARE(pins, QStringList({ec, rsa}));
    QCOMPARE(rejected, QStringList({QStringLiteral("not-a-pin")}));
    QVERIFY(PlankBroker::parsePinList(QString()).isEmpty());
    QCOMPARE(PlankBroker::normalizePins({rsa.toUpper(), QStringLiteral("x"), rsa}), QStringList({rsa}));
}

void TestPlankBroker::shipsCanonicalDefaultPins()
{
    const QStringList pins = PlankBroker::defaultPins();
    QCOMPARE(pins.size(), 2); // current + spare
    for (const QString& pin : pins) QVERIFY(PlankBroker::isCanonicalSha256Hex(pin));
    QCOMPARE(PlankBroker::normalizePins(pins), pins);
    QCOMPARE(PlankBroker::defaultHost(), QStringLiteral("remote.bde.run"));
    QCOMPARE(PlankBroker::DefaultPort, quint16(29000));
}

void TestPlankBroker::extractsSpkiFromEcAndRsaCertificates()
{
    QCOMPARE(PlankBroker::spkiSha256Hex(ecCertificate()), QString::fromLatin1(EcSpkiSha256));
    QCOMPARE(PlankBroker::spkiSha256Hex(rsaCertificate()), QString::fromLatin1(RsaSpkiSha256));
    // The SPKI is a DER SEQUENCE inside the certificate.
    const QByteArray spki = PlankBroker::subjectPublicKeyInfoDer(ecCertificate());
    QVERIFY(!spki.isEmpty());
    QCOMPARE(static_cast<quint8>(spki.at(0)), quint8(0x30));
    QVERIFY(ecCertificate().contains(spki));
    // Qt's own parser agrees on the key bytes it exposes.
    const QSslCertificate qtCertificate(ecCertificate(), QSsl::Der);
    QVERIFY(!qtCertificate.isNull());
    QCOMPARE(PlankBroker::spkiSha256Hex(qtCertificate.toDer()), QString::fromLatin1(EcSpkiSha256));
}

void TestPlankBroker::spkiPinMatches()
{
    QVERIFY(PlankBroker::spkiPinMatches(ecCertificate(), {QString::fromLatin1(EcSpkiSha256)}));
    QVERIFY(PlankBroker::spkiPinMatches(ecCertificate(), {QString::fromLatin1(EcSpkiSha256).toUpper()}));
    QVERIFY(PlankBroker::spkiPinMatches(rsaCertificate(), {QString::fromLatin1(RsaSpkiSha256)}));
}

void TestPlankBroker::spkiPinMismatchIsRejected()
{
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), {QString::fromLatin1(RsaSpkiSha256)}));
    QVERIFY(!PlankBroker::spkiPinMatches(rsaCertificate(), {QString::fromLatin1(EcSpkiSha256)}));
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), PlankBroker::defaultPins()));
    // A leaf hash is not an SPKI hash.
    QVERIFY(!PlankBroker::spkiPinMatches(rsaCertificate(), {QString::fromLatin1(RsaLeafSha256)}));
}

void TestPlankBroker::spkiMatchesAnyOfMultiplePins()
{
    const QStringList currentAndSpare = {QString::fromLatin1(RsaSpkiSha256), QString::fromLatin1(EcSpkiSha256)};
    QVERIFY(PlankBroker::spkiPinMatches(ecCertificate(), currentAndSpare));
    QVERIFY(PlankBroker::spkiPinMatches(rsaCertificate(), currentAndSpare));
    QVERIFY(PlankBroker::spkiPinMatches(ecCertificate(),
                                        {QStringLiteral("garbage"), QString::fromLatin1(EcSpkiSha256)}));
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), PlankBroker::defaultPins() + QStringList {
        QString::fromLatin1(RsaSpkiSha256)}));
}

void TestPlankBroker::emptyOrInvalidPinsNeverMatch()
{
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), {}));
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), {QString()}));
    QVERIFY(!PlankBroker::spkiPinMatches(ecCertificate(), {QString::fromLatin1(EcSpkiSha256).left(32)}));
}

void TestPlankBroker::malformedCertificatesAreRejected()
{
    const QByteArray certificate = ecCertificate();
    QVERIFY(PlankBroker::subjectPublicKeyInfoDer(QByteArray()).isEmpty());
    QVERIFY(PlankBroker::subjectPublicKeyInfoDer(certificate.left(certificate.size() - 1)).isEmpty());
    QVERIFY(PlankBroker::subjectPublicKeyInfoDer(certificate + QByteArray(1, '\0')).isEmpty());
    QByteArray wrongTag = certificate;
    wrongTag[0] = 0x31;
    QVERIFY(PlankBroker::subjectPublicKeyInfoDer(wrongTag).isEmpty());
    QVERIFY(!PlankBroker::spkiPinMatches(wrongTag, {QString::fromLatin1(EcSpkiSha256)}));
    QVERIFY(PlankBroker::subjectPublicKeyInfoDer(QByteArray("\x30\x84\xff\xff\xff\xff", 6)).isEmpty());
}

void TestPlankBroker::hostLeafPin()
{
    QVERIFY(PlankBroker::hostLeafMatches(rsaCertificate(), QString::fromLatin1(RsaLeafSha256)));
    QVERIFY(!PlankBroker::hostLeafMatches(ecCertificate(), QString::fromLatin1(RsaLeafSha256)));
    // SPKI hash is not accepted where the leaf hash is required.
    QVERIFY(!PlankBroker::hostLeafMatches(rsaCertificate(), QString::fromLatin1(RsaSpkiSha256)));
    QVERIFY(!PlankBroker::hostLeafMatches(rsaCertificate(), QString::fromLatin1(RsaLeafSha256).toUpper()));
    QVERIFY(!PlankBroker::hostLeafMatches(rsaCertificate(), QString()));
    QVERIFY(!PlankBroker::hostLeafMatches(QByteArray(), QString::fromLatin1(RsaLeafSha256)));
}

void TestPlankBroker::parsesChallenge()
{
    const auto reply = PlankBroker::parseAuthReply(200, json(R"({"state":"challenge","conversation_id":"c-1",
        "prompts":[{"id":"password","style":"secret","text":"Password"},
                   {"id":"otp","style":"otp","text":"Authenticator code"}]})"));
    QCOMPARE(reply.kind, PlankBroker::ReplyKind::Challenge);
    QCOMPARE(reply.conversationId, QStringLiteral("c-1"));
    QCOMPARE(reply.prompts.size(), 2);
    QCOMPARE(reply.prompts.at(0).id, QStringLiteral("password"));
    QCOMPARE(reply.prompts.at(0).style, QStringLiteral("secret"));
    QCOMPARE(reply.prompts.at(1).style, QStringLiteral("otp"));
    QCOMPARE(reply.prompts.at(1).text, QStringLiteral("Authenticator code"));
}

void TestPlankBroker::parsesAuthenticated()
{
    const auto reply = PlankBroker::parseAuthReply(200, json(R"({"state":"authenticated",
        "session_token":"AbC-_123","expires_in":36000,"username":"anna"})"));
    QCOMPARE(reply.kind, PlankBroker::ReplyKind::Authenticated);
    QCOMPARE(reply.sessionToken, QStringLiteral("AbC-_123"));
    QCOMPARE(reply.expiresIn, 36000);
    QCOMPARE(reply.username, QStringLiteral("anna"));
}

void TestPlankBroker::parsesDenied()
{
    QCOMPARE(PlankBroker::parseAuthReply(200, json(R"({"state":"denied"})")).kind,
             PlankBroker::ReplyKind::Denied);
    // Extra fields do not turn a denial into anything else.
    QCOMPARE(PlankBroker::parseAuthReply(200, json(R"({"state":"denied","session_token":"x"})")).kind,
             PlankBroker::ReplyKind::Denied);
}

void TestPlankBroker::parsesRateLimit()
{
    auto reply = PlankBroker::parseAuthReply(429, json(R"({"state":"denied","retry_after":900})"));
    QCOMPARE(reply.kind, PlankBroker::ReplyKind::RateLimited);
    QCOMPARE(reply.retryAfter, 900);
    reply = PlankBroker::parseAuthReply(429, json("not json"));
    QCOMPARE(reply.kind, PlankBroker::ReplyKind::RateLimited);
    QCOMPARE(reply.retryAfter, PlankBroker::DefaultRetryAfterSeconds);
    QCOMPARE(PlankBroker::parseAuthReply(429, json(R"({"retry_after":1e9})")).retryAfter,
             PlankBroker::MaximumRetryAfterSeconds);
    QCOMPARE(PlankBroker::parseAuthReply(429, json(R"({"retry_after":-5})")).retryAfter, 1);
    QCOMPARE(PlankBroker::parseAuthReply(429, json(R"({"retry_after":"soon"})")).retryAfter,
             PlankBroker::DefaultRetryAfterSeconds);
}

void TestPlankBroker::rejectsMalformedAuthReplies()
{
    const char* malformed[] = {
        "",
        "[]",
        "{not json",
        R"({"state":"ok"})",
        R"({"state":"challenge","prompts":[{"id":"p","style":"secret","text":"P"}]})",
        R"({"state":"challenge","conversation_id":"c","prompts":[]})",
        R"({"state":"challenge","conversation_id":"c","prompts":[{"id":"p","style":"voice","text":"P"}]})",
        R"({"state":"challenge","conversation_id":"c","prompts":["Password"]})",
        R"({"state":"challenge","conversation_id":"has space","prompts":[{"id":"p","style":"secret","text":"P"}]})",
        R"({"state":"authenticated"})",
        R"({"state":"authenticated","session_token":"tok en"})",
        R"({"state":"authenticated","session_token":"t","expires_in":"never"})",
    };
    for (const char* body : malformed) {
        QCOMPARE(PlankBroker::parseAuthReply(200, json(body)).kind, PlankBroker::ReplyKind::Malformed);
    }
    QCOMPARE(PlankBroker::parseAuthReply(500, json(R"({"state":"authenticated","session_token":"t"})")).kind,
             PlankBroker::ReplyKind::Malformed);
    QCOMPARE(PlankBroker::parseAuthReply(200, QByteArray(PlankBroker::MaximumReplyBytes + 1, ' ')).kind,
             PlankBroker::ReplyKind::Malformed);
}

void TestPlankBroker::answersPrompts()
{
    const QVector<PlankBroker::Prompt> prompts = {
        {QStringLiteral("password"), QStringLiteral("secret"), QStringLiteral("Password")},
        {QStringLiteral("otp"), QStringLiteral("otp"), QStringLiteral("Authenticator code")},
    };
    QJsonArray responses;
    QVERIFY(PlankBroker::buildResponses(prompts, QStringLiteral("anna"), QStringLiteral("pw"),
                                        QStringLiteral("012345"), responses));
    QCOMPARE(responses, QJsonArray({QStringLiteral("pw"), QStringLiteral("012345")}));

    QVERIFY(!PlankBroker::buildResponses(prompts, QStringLiteral("anna"), QStringLiteral("pw"),
                                         QStringLiteral("12345"), responses));
    QVERIFY(!PlankBroker::buildResponses(prompts, QStringLiteral("anna"), QStringLiteral("pw"),
                                         QStringLiteral("12345a"), responses));
    QVERIFY(!PlankBroker::buildResponses(prompts, QStringLiteral("anna"), QString(),
                                         QStringLiteral("123456"), responses));
    const QVector<PlankBroker::Prompt> mixed = {
        {QStringLiteral("user"), QStringLiteral("text"), QStringLiteral("Login")},
        {QStringLiteral("note"), QStringLiteral("info"), QStringLiteral("Hi")},
    };
    QVERIFY(PlankBroker::buildResponses(mixed, QStringLiteral("anna"), QString(), QString(), responses));
    QCOMPARE(responses, QJsonArray({QStringLiteral("anna"), QString()}));
    QVERIFY(!PlankBroker::buildResponses({{QStringLiteral("x"), QStringLiteral("voice"), QString()}},
                                         QStringLiteral("anna"), QStringLiteral("pw"),
                                         QStringLiteral("123456"), responses));
    QVERIFY(responses.isEmpty());
    QVERIFY(PlankBroker::isValidOtp(QStringLiteral("000000")));
    QVERIFY(!PlankBroker::isValidOtp(QStringLiteral("1234567")));
}

void TestPlankBroker::parsesHosts()
{
    QVector<PlankBroker::Host> hosts;
    QVERIFY(PlankBroker::parseHosts(json(R"({"hosts":[
        {"id":"ws01.example.test","name":"ws01","online":true,"in_use_by":null,"connectable":true,"reason":null},
        {"id":"ws02.example.test","name":"ws02","online":true,"in_use_by":"bob","connectable":false,"reason":"in use"},
        {"id":"ws03.example.test","online":false,"connectable":false,"reason":"offline"}]})"), hosts));
    QCOMPARE(hosts.size(), 3);
    QCOMPARE(hosts.at(0).id, QStringLiteral("ws01.example.test"));
    QCOMPARE(hosts.at(0).name, QStringLiteral("ws01"));
    QVERIFY(hosts.at(0).online);
    QVERIFY(hosts.at(0).inUseBy.isEmpty());
    QVERIFY(hosts.at(0).connectable);
    QCOMPARE(hosts.at(1).inUseBy, QStringLiteral("bob"));
    QCOMPARE(hosts.at(1).reason, QStringLiteral("in use"));
    QVERIFY(!hosts.at(1).connectable);
    QCOMPARE(hosts.at(2).name, QStringLiteral("ws03.example.test")); // falls back to id
    QVERIFY(!hosts.at(2).online);
    QVERIFY(PlankBroker::parseHosts(json(R"({"hosts":[]})"), hosts));
    QVERIFY(hosts.isEmpty());
}

void TestPlankBroker::rejectsMalformedHosts()
{
    QVector<PlankBroker::Host> hosts = {PlankBroker::Host()};
    const char* malformed[] = {
        "",
        R"({"hosts":{}})",
        R"({})",
        R"({"hosts":[{"id":"ws01","online":"yes","connectable":true}]})",
        R"({"hosts":[{"id":"ws01","online":true}]})",
        R"({"hosts":[{"id":"../etc","online":true,"connectable":true}]})",
        R"({"hosts":[{"id":"ws01","online":true,"connectable":true,"in_use_by":42}]})",
        R"({"hosts":[{"id":"ws01","online":true,"connectable":true}, 7]})",
    };
    for (const char* body : malformed) {
        QVERIFY2(!PlankBroker::parseHosts(json(body), hosts), body);
        QVERIFY(hosts.isEmpty());
    }
}

void TestPlankBroker::parsesLease()
{
    PlankBroker::Lease lease;
    QVERIFY(PlankBroker::parseLease(json(R"({"endpoint":"remote.bde.run","port":29042,
        "host_cert_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "username":"anna","gssapi_token":"YIIBAgYJKoZIhvcSAQICAQBuggE=","expires_in":30})"), lease));
    QCOMPARE(lease.endpoint, QStringLiteral("remote.bde.run"));
    QCOMPARE(lease.port, quint16(29042));
    QCOMPARE(lease.hostCertSha256, HostPin);
    QCOMPARE(lease.username, QStringLiteral("anna"));
    QCOMPARE(lease.gssapiToken, QStringLiteral("YIIBAgYJKoZIhvcSAQICAQBuggE="));
    QCOMPARE(lease.expiresIn, 30);
}

void TestPlankBroker::rejectsMalformedLeases()
{
    const QString good = QStringLiteral(R"({"endpoint":"remote.bde.run","port":29042,
        "host_cert_sha256":"%1","username":"anna","gssapi_token":"YWJj","expires_in":30})").arg(HostPin);
    PlankBroker::Lease lease;
    QVERIFY(PlankBroker::parseLease(good.toUtf8(), lease));
    const QList<QPair<QString, QString>> mutations = {
        {QStringLiteral("\"port\":29042"), QStringLiteral("\"port\":0")},
        {QStringLiteral("\"port\":29042"), QStringLiteral("\"port\":70000")},
        {QStringLiteral("\"port\":29042"), QStringLiteral("\"port\":\"29042\"")},
        {QStringLiteral("\"port\":29042"), QStringLiteral("\"port\":29042.5")},
        {HostPin, HostPin.toUpper()},
        {HostPin, HostPin.left(62)},
        {QStringLiteral("\"YWJj\""), QStringLiteral("\"not base64!\"")},
        {QStringLiteral("\"YWJj\""), QStringLiteral("\"\"")},
        {QStringLiteral("remote.bde.run"), QStringLiteral("remote.bde.run/evil")},
        {QStringLiteral("remote.bde.run"), QStringLiteral("")},
        {QStringLiteral("\"anna\""), QStringLiteral("\"\"")},
        {QStringLiteral("\"expires_in\":30"), QStringLiteral("\"expires_in\":0")},
    };
    for (const auto& mutation : mutations) {
        QString body = good;
        body.replace(mutation.first, mutation.second);
        QVERIFY2(!PlankBroker::parseLease(body.toUtf8(), lease), qPrintable(body));
        QCOMPARE(lease.port, quint16(0));
        QVERIFY(lease.gssapiToken.isEmpty());
    }
}

void TestPlankBroker::parsesLeaseRoute()
{
    const QString body = QStringLiteral(R"({%1"endpoint":"%2","port":%3,
        "host_cert_sha256":"%4","username":"anna","gssapi_token":"YWJj","expires_in":30})");
    PlankBroker::Lease lease;
    // Older brokers send no route: relayed.
    QVERIFY(PlankBroker::parseLease(body.arg(QString(), QStringLiteral("remote.bde.run"),
                                             QStringLiteral("29042"), HostPin).toUtf8(), lease));
    QCOMPARE(lease.route, PlankBroker::Route::Relay);
    QVERIFY(PlankBroker::parseLease(body.arg(QStringLiteral(R"("route":"relay",)"), QStringLiteral("remote.bde.run"),
                                             QStringLiteral("29042"), HostPin).toUtf8(), lease));
    QCOMPARE(lease.route, PlankBroker::Route::Relay);
    // Direct: the workstation's own IPv4 address and PLANK port.
    QVERIFY(PlankBroker::parseLease(body.arg(QStringLiteral(R"("route":"direct",)"), QStringLiteral("192.168.10.57"),
                                             QStringLiteral("28989"), HostPin).toUtf8(), lease));
    QCOMPARE(lease.route, PlankBroker::Route::Direct);
    QCOMPARE(lease.endpoint, QStringLiteral("192.168.10.57"));
    QCOMPARE(lease.port, quint16(28989));
    for (const char* route : {R"("route":"tunnel",)", R"("route":1,)", R"("route":null,)"}) {
        QVERIFY2(!PlankBroker::parseLease(body.arg(QString::fromLatin1(route), QStringLiteral("remote.bde.run"),
                                                   QStringLiteral("29042"), HostPin).toUtf8(), lease), route);
        QCOMPARE(lease.route, PlankBroker::Route::Relay);
        QVERIFY(lease.gssapiToken.isEmpty());
    }
}

void TestPlankBroker::classifiesBearerStatus()
{
    using PlankBroker::BearerStatus;
    QCOMPARE(PlankBroker::classifyBearerStatus(200), BearerStatus::Ok);
    QCOMPARE(PlankBroker::classifyBearerStatus(204), BearerStatus::Ok);
    QCOMPARE(PlankBroker::classifyBearerStatus(401), BearerStatus::SessionExpired);
    QCOMPARE(PlankBroker::classifyBearerStatus(429), BearerStatus::RateLimited);
    QCOMPARE(PlankBroker::classifyBearerStatus(403), BearerStatus::Denied);
    QCOMPARE(PlankBroker::classifyBearerStatus(404), BearerStatus::Denied);
    QCOMPARE(PlankBroker::classifyBearerStatus(409), BearerStatus::Denied);
    QCOMPARE(PlankBroker::classifyBearerStatus(302), BearerStatus::Malformed);
    QCOMPARE(PlankBroker::classifyBearerStatus(500), BearerStatus::Malformed);
}

void TestPlankBroker::encodesHostActionPath()
{
    QCOMPARE(PlankBroker::hostActionPath(QStringLiteral("ws01.example.test"), QStringLiteral("connect")),
             QStringLiteral("/v1/hosts/ws01.example.test/connect"));
    QCOMPARE(PlankBroker::hostActionPath(QStringLiteral("a/b"), QStringLiteral("keepalive")),
             QStringLiteral("/v1/hosts/a%2Fb/keepalive"));
    QVERIFY(PlankBroker::isHostId(QStringLiteral("ws01.example.test")));
    QVERIFY(!PlankBroker::isHostId(QStringLiteral("a/b")));
    QVERIFY(!PlankBroker::isHostId(QStringLiteral("-lead")));
}

void TestPlankBroker::parsesHostAdmission()
{
    QString token;
    QCOMPARE(PlankBroker::parseHostAdmission({{"state", "authenticated"}, {"session_token", "abc"}}, token),
             PlankBroker::AdmissionResult::Authenticated);
    QCOMPARE(token, QStringLiteral("abc"));
    QCOMPARE(PlankBroker::parseHostAdmission({{"state", "denied"}}, token), PlankBroker::AdmissionResult::Denied);
    QVERIFY(token.isEmpty());
    QCOMPARE(PlankBroker::parseHostAdmission({{"state", "busy"}}, token), PlankBroker::AdmissionResult::Busy);
    // Brokered mode never answers a PAM password challenge.
    QCOMPARE(PlankBroker::parseHostAdmission({{"state", "challenge"}, {"conversation_id", "c"},
                                              {"messages", QJsonArray()}}, token),
             PlankBroker::AdmissionResult::Malformed);
    QCOMPARE(PlankBroker::parseHostAdmission({{"state", "authenticated"}}, token),
             PlankBroker::AdmissionResult::Malformed);
}

void TestPlankBroker::clientRefusesWithoutPins()
{
    PlankBrokerClient::Config config;
    config.host = PlankBroker::defaultHost();
    config.port = PlankBroker::DefaultPort;
    config.pins = {};
    PlankBrokerClient client(config);
    try {
        client.start(QStringLiteral("anna"));
        QFAIL("A broker client without pins must refuse to connect");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::NotConfigured);
    }
    config.pins = {QStringLiteral("invalid")};
    try {
        PlankBrokerClient(config).hosts(QStringLiteral("token"));
        QFAIL("Only invalid pins must count as no pins");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::NotConfigured);
    }
    config.pins = PlankBroker::defaultPins();
    config.host = QStringLiteral("bad host/");
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, PlankBrokerClient(config).checkConfigured());
    config.host = PlankBroker::defaultHost();
    PlankBrokerClient(config).checkConfigured();
}

void TestPlankBroker::errorMessagesAreGeneric()
{
    QVERIFY(PlankBrokerError(PlankBrokerError::RateLimited, 42).userMessage().contains(QStringLiteral("42")));
    QCOMPARE(PlankBrokerError(PlankBrokerError::Denied).userMessage(), QStringLiteral("Sign-in failed."));
    QVERIFY(PlankBrokerError(PlankBrokerError::NotConfigured).userMessage().contains(QStringLiteral("Settings")));
    for (auto kind : {PlankBrokerError::Network, PlankBrokerError::Tls, PlankBrokerError::SessionExpired,
                      PlankBrokerError::Protocol}) {
        QVERIFY(!PlankBrokerError(kind).userMessage().isEmpty());
    }
}

void TestPlankBroker::brokeredTransportUsesLeasedPort()
{
    PlankBroker::TransportTarget target;
    // Host advertises its own 28989; the lease is 29042.
    QVERIFY(PlankBroker::resolveTransportTarget(HostPin, 29042, 28989, HostPin, target));
    QCOMPARE(target.port, quint16(29042));
    QCOMPARE(target.certificateSha256, HostPin);
    // A launch reply without its own transport certificate still pins the lease leaf.
    QVERIFY(PlankBroker::resolveTransportTarget(HostPin, 29042, 0, QString(), target));
    QCOMPARE(target.port, quint16(29042));
    QCOMPARE(target.certificateSha256, HostPin);
}

void TestPlankBroker::brokeredTransportRequiresPinnedCertificate()
{
    PlankBroker::TransportTarget target;
    const QString other = QString(HostPin).replace(0, 1, QStringLiteral("f"));
    QVERIFY(!PlankBroker::resolveTransportTarget(HostPin, 29042, 28989, other, target));
    QCOMPARE(target.port, quint16(0));
    QVERIFY(!PlankBroker::resolveTransportTarget(QStringLiteral("not-a-pin"), 29042, 28989, HostPin, target));
    QVERIFY(!PlankBroker::resolveTransportTarget(HostPin, 0, 28989, HostPin, target));
}

void TestPlankBroker::directTransportIsUnchanged()
{
    PlankBroker::TransportTarget target;
    QVERIFY(PlankBroker::resolveTransportTarget(QString(), 28989, 28989, HostPin, target));
    QCOMPARE(target.port, quint16(28989));
    QVERIFY(PlankBroker::resolveTransportTarget(QString(), 28989, 30000, HostPin, target));
    QCOMPARE(target.port, quint16(30000));
    QVERIFY(!PlankBroker::resolveTransportTarget(QString(), 28989, 0, HostPin, target));
    QVERIFY(!PlankBroker::resolveTransportTarget(QString(), 28989, 28989, QString(), target));
}

void TestPlankBroker::brokeredMacLaunchIgnoresUdpPort()
{
    NvOutputTopology topology;
    QVERIFY(NvOutputTopology::fromJson(fixedCaptureTopology(), topology));
    QJsonObject reply {
        {"schema_version", 2}, {"state", "connecting"},
        {"transport_token", QString::fromLatin1(QByteArray(32, 'k').toBase64())},
        {"udp_port", 28989}, {"max_udp_payload_size", 1200},
        {"capture", topology.toJson().value("capture")},
        {"services", QJsonObject {{"audio", true}, {"input", true}, {"pen", "normalized"},
                                  {"cursor", "embedded"}}},
    };
    MacPreviewLaunch::Reply parsed;
    // Direct: udp_port must equal the approved control port.
    QVERIFY(!MacPreviewLaunch::parseReply(reply, topology, 29042, 1200, parsed));
    QVERIFY(MacPreviewLaunch::parseReply(reply, topology, 28989, 1200, parsed));
    QCOMPARE(parsed.configuration.sessionPort, 28989u);
    // Brokered: the advertised 28989 is ignored; QUIC uses the leased port.
    QVERIFY(MacPreviewLaunch::parseReply(reply, topology, 29042, 1200, parsed, true));
    QCOMPARE(parsed.configuration.sessionPort, 29042u);
    reply["udp_port"] = 0;
    QVERIFY(!MacPreviewLaunch::parseReply(reply, topology, 29042, 1200, parsed, true));
    reply["udp_port"] = "28989";
    QVERIFY(!MacPreviewLaunch::parseReply(reply, topology, 29042, 1200, parsed, true));
}

void TestPlankBroker::keepaliveCadence()
{
    PlankBroker::KeepaliveSchedule schedule;
    QCOMPARE(schedule.nextDelayMs(0), qint64(-1)); // not started
    schedule.start(1000);
    QVERIFY(schedule.active());
    const qint64 first = schedule.nextDelayMs(1000);
    QVERIFY(first >= 30000 && first <= 45000);
    QVERIFY(first + PlankBroker::KeepaliveSchedule::SafetyMarginMs < PlankBroker::KeepaliveSchedule::LeaseWindowMs);
    QCOMPARE(schedule.nextDelayMs(21000), first - 20000);
    QCOMPARE(schedule.nextDelayMs(40000), qint64(0)); // overdue: send now
    schedule.recordSuccess(40000);
    QCOMPARE(schedule.nextDelayMs(40000), first);
    schedule.stop();
    QCOMPARE(schedule.nextDelayMs(40000), qint64(-1));
}

void TestPlankBroker::keepaliveRetriesThenGivesUp()
{
    PlankBroker::KeepaliveSchedule schedule;
    schedule.start(0);
    schedule.recordFailure(); // at t=30 s
    QCOMPARE(schedule.nextDelayMs(30000), PlankBroker::KeepaliveSchedule::RetryMs);
    schedule.recordFailure(); // t=40 s
    QCOMPARE(schedule.nextDelayMs(40000), PlankBroker::KeepaliveSchedule::RetryMs);
    schedule.recordFailure(); // t=50 s: only 5 s left before the safety margin
    QCOMPARE(schedule.nextDelayMs(50000), qint64(5000));
    QCOMPARE(schedule.nextDelayMs(55000), qint64(-1)); // lease must be assumed lost
    QCOMPARE(schedule.failures(), 3);
    schedule.recordSuccess(54000); // a late success restores the cadence
    QCOMPARE(schedule.failures(), 0);
    QCOMPARE(schedule.nextDelayMs(54000), PlankBroker::KeepaliveSchedule::IntervalMs);
}

void TestPlankBroker::tlsPinnedRoundTrip()
{
    qInfo() << "TLS backend:" << QSslSocket::activeBackend();
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = R"({"hosts":[{"id":"ws01.example.test","name":"ws01","online":true,)"
                  R"("in_use_by":null,"connectable":true,"reason":null}]})";
    // Spare pin listed first: any configured pin may match. ECDSA P-256 is
    // accepted for the broker (the RSA-3072 profile applies to hosts only).
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(RsaSpkiSha256),
                                                              QString::fromLatin1(EcSpkiSha256)}));
    const auto hosts = client.hosts(QStringLiteral("session-token"));
    QCOMPARE(hosts.size(), 1);
    QCOMPARE(hosts.at(0).name, QStringLiteral("ws01"));
    QVERIFY(server.request.startsWith("GET /v1/hosts HTTP/1.1\r\n"));
    QVERIFY(server.request.contains("Authorization: Bearer session-token\r\n"));

    server.request.clear();
    server.body = R"({"state":"challenge","conversation_id":"c1","prompts":[)"
                  R"({"id":"password","style":"secret","text":"Password"},)"
                  R"({"id":"otp","style":"otp","text":"Authenticator code"}]})";
    const auto challenge = client.start(QStringLiteral("anna"));
    QCOMPARE(challenge.kind, PlankBroker::ReplyKind::Challenge);
    QVERIFY(server.request.startsWith("POST /v1/auth/start HTTP/1.1\r\n"));
    QVERIFY(server.request.endsWith("{\"username\":\"anna\"}"));
    QVERIFY(!server.request.contains("Authorization:"));
}

void TestPlankBroker::tlsWrongPinSendsNothing()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = R"({"hosts":[]})";
    try {
        PlankBrokerClient(localConfig(server.port(), {QString::fromLatin1(RsaSpkiSha256)}))
                .hosts(QStringLiteral("secret-session-token"));
        QFAIL("A broker whose key matches no pin must be rejected");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::Tls);
    }
    QCoreApplication::processEvents();
    QVERIFY(!server.request.contains("secret-session-token"));
    QVERIFY(server.request.isEmpty());
}

void TestPlankBroker::tlsRateLimitOnBearerCall()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.status = "429 Too Many Requests";
    server.body = R"({"state":"denied","retry_after":120})";
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    try {
        client.start(QStringLiteral("anna"));
        QFAIL("429 must throw");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::RateLimited);
        QCOMPARE(error.retryAfter(), 120);
    }
    server.status = "401 Unauthorized";
    server.body = R"({"state":"denied"})";
    try {
        client.connect(QStringLiteral("t"), QStringLiteral("ws01.example.test"));
        QFAIL("401 must throw");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::SessionExpired);
    }
}

void TestPlankBroker::tlsConnectAsksForRelayOnlyWhenForced()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = QStringLiteral(R"({"route":"direct","endpoint":"192.168.10.57","port":28989,)"
                                 R"("host_cert_sha256":"%1","username":"anna","gssapi_token":"YWJj",)"
                                 R"("expires_in":30})").arg(HostPin).toUtf8();
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const PlankBroker::Lease direct = client.connect(QStringLiteral("t"), QStringLiteral("ws01.example.test"));
    QCOMPARE(direct.route, PlankBroker::Route::Direct);
    QVERIFY(server.request.startsWith("POST /v1/hosts/ws01.example.test/connect HTTP/1.1\r\n"));
    QVERIFY(server.request.endsWith("{}"));

    server.request.clear();
    server.body = QStringLiteral(R"({"route":"relay","endpoint":"remote.bde.run","port":29042,)"
                                 R"("host_cert_sha256":"%1","username":"anna","gssapi_token":"YWJj",)"
                                 R"("expires_in":30})").arg(HostPin).toUtf8();
    const PlankBroker::Lease relayed = client.connect(QStringLiteral("t"), QStringLiteral("ws01.example.test"), true);
    QCOMPARE(relayed.route, PlankBroker::Route::Relay);
    QCOMPARE(relayed.port, quint16(29042));
    QVERIFY(server.request.endsWith("{\"route\":\"relay\"}"));
}

void TestPlankBroker::tlsRequiresTls13()
{
    TestBrokerServer server(QSsl::TlsV1_2);
    QVERIFY(server.listen());
    server.body = R"({"hosts":[]})";
    try {
        PlankBrokerClient(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}))
                .hosts(QStringLiteral("t"));
        QFAIL("A TLS 1.2-only broker must be rejected");
    } catch (const PlankBrokerError& error) {
        QVERIFY(error.kind() == PlankBrokerError::Tls || error.kind() == PlankBrokerError::Network);
    }
    QVERIFY(!server.request.contains("Authorization"));
}

QTEST_GUILESS_MAIN(TestPlankBroker)
#include "test_plankbroker.moc"

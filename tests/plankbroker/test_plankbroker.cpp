#include <memory>
#include <QtTest>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>

#include <memory>

#include "plankbroker.h"
#include "plankhttp.h"
#include "remotedisplaysetup.h"
#include "displayprofile.h"
#include "clientdisplayprobe.h"
#include "remotestreamsetup.h"
#include "brokersessionstore.h"
#include <QTemporaryDir>
#include <QNetworkProxy>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include "plankbrokerclient.h"
#include "plankpasskey.h"
#include "macpreviewlaunch.h"
#include "outputtopology.h"
#include "plankenrollment.h"
#include "onboardingstate.h"
#include "qrencoder.h"

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

// Queue this status to read the request and drop the connection unanswered.
const QByteArray DropReply = QByteArrayLiteral("DROP");

// Minimal HTTPS/1.1 responder: one canned reply per connection, records the
// raw request bytes it received (to prove nothing is sent to a wrong peer).
// Queued replies (`replies`, status + body) are served first, one per
// request, then `status`/`body`; every complete request is kept in `requestLog`.
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
                ++connections;
                auto pending = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, pending]() {
                    const QByteArray chunk = socket->readAll();
                    request += chunk;
                    *pending += chunk;
                    const int headerEnd = pending->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int contentLength = 0;
                    for (const QByteArray& line : pending->left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(15).trimmed().toInt();
                        }
                    }
                    if (pending->size() < headerEnd + 4 + contentLength) return;
                    ++requests;
                    requestLog.append(*pending);
                    pending->clear();
                    QByteArray replyStatus = status;
                    QByteArray replyBody = body;
                    if (!replies.isEmpty()) {
                        const auto next = replies.takeFirst();
                        replyStatus = next.first;
                        replyBody = next.second;
                    }
                    if (replyStatus == DropReply) {
                        // The request arrived, but its reply is lost.
                        socket->abort();
                        return;
                    }
                    // announceClose=false mimics the PLANK host: HTTP/1.1 without
                    // "Connection: close", yet the socket is closed after the reply.
                    socket->write("HTTP/1.1 " + replyStatus + "\r\nContent-Type: application/json\r\n"
                                  "Cache-Control: no-store\r\n" +
                                  QByteArray(announceClose ? "Connection: close\r\n" : "") +
                                  "Content-Length: " + QByteArray::number(replyBody.size()) + "\r\n\r\n" + replyBody);
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
    bool announceClose = true;
    int connections = 0;
    int requests = 0;
    // Queued replies (status, body) are served first, one per request;
    // every complete request is kept in requestLog.
    QList<QPair<QByteArray, QByteArray>> replies;
    QList<QByteArray> requestLog;

    void queue(const QByteArray& replyStatus, const QByteArray& replyBody)
    {
        replies.append(qMakePair(replyStatus, replyBody));
    }

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

// Section 13.3 fixtures: a 32-byte challenge and two credential ids.
const QString PasskeyChallenge = QString::fromLatin1(QByteArray(32, '\x5a').toBase64());
const QString PasskeyCredential = QString::fromLatin1(QByteArray(32, '\x11').toBase64());
const QString PasskeyOtherCredential = QString::fromLatin1(QByteArray(32, '\x22').toBase64());

QByteArray passkeyChallengeReply(const QString& rpId = QStringLiteral("ipa.bde.run"))
{
    return QString::fromUtf8(R"({"state":"challenge","conversation_id":"pk-1","prompts":[{"id":"passkey",)"
                          R"("style":"passkey","text":"Passkey","passkey":{"rp_id":"%1","credential_ids":["%2","%3"],)"
                          R"("user_verification":true,"challenge":"%4"}}]})")
            .arg(rpId, PasskeyOtherCredential, PasskeyCredential, PasskeyChallenge).toUtf8();
}

// authData = SHA-256(rp_id) || flags || counter, built independently here.
QByteArray passkeyAuthData(const QString& rpId, quint8 flags = 0x05)
{
    QByteArray data = QCryptographicHash::hash(rpId.toUtf8(), QCryptographicHash::Sha256);
    data.append(char(flags));
    data.append(QByteArray(4, '\0'));
    return data;
}

PlankBroker::PasskeyRequest passkeyRequest()
{
    PlankBroker::PasskeyRequest request;
    request.rpId = QStringLiteral("ipa.bde.run");
    request.credentialIds = {PasskeyOtherCredential, PasskeyCredential};
    request.challenge = PasskeyChallenge;
    return request;
}

QByteArray assertionJson(const QString& credentialId, const QByteArray& authData, const QByteArray& signature)
{
    return QJsonDocument(QJsonObject {
        {QStringLiteral("credential_id"), credentialId},
        {QStringLiteral("authenticator_data"), QString::fromLatin1(authData.toBase64())},
        {QStringLiteral("signature"), QString::fromLatin1(signature.toBase64())},
    }).toJson(QJsonDocument::Compact);
}

// A stand-in for plank-passkey: records argv and stdin, prints `output`, exits `code`.
QString fakeHelper(const QTemporaryDir& directory, int code, const QByteArray& output)
{
    const QString path = directory.filePath(QStringLiteral("plank-passkey-%1").arg(code));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return QString();
    QFile outputFile(path + QStringLiteral(".out"));
    if (!outputFile.open(QIODevice::WriteOnly)) return QString();
    outputFile.write(output);
    file.write(QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" > '%1.args'\ncat > '%1.stdin'\n"
                              "cat '%1.out'\nexit %2\n").arg(path).arg(code).toUtf8());
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A raw HTTP/1.1 request as TestBrokerServer recorded it.
struct RecordedRequest {
    QByteArray method;
    QByteArray target;
    QMap<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
};

RecordedRequest parseRecordedRequest(const QByteArray& raw)
{
    RecordedRequest parsed;
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) return parsed;
    const QList<QByteArray> lines = raw.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    parsed.method = requestLine.value(0);
    parsed.target = requestLine.value(1);
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines.at(i).indexOf(':');
        if (colon > 0) parsed.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
    }
    parsed.body = raw.mid(headerEnd + 4);
    return parsed;
}

// A DER-shaped stand-in signature (the client only checks its shape).
QString fakeDeviceSignature(int serial)
{
    QByteArray der = QByteArray::fromHex("3044022001020304050607080910111213141516171819202122232425262728293031"
                                         "3202200102030405060708091011121314151617181920212223242526272829303132");
    der[5] = char(serial);
    return QString::fromLatin1(der.toBase64());
}

// A P-256 SPKI DER prefix plus a fake point: the shape the helper prints.
const QString DeviceSpki = QString::fromLatin1(
        (QByteArray::fromHex("3059301306072a8648ce3d020106082a8648ce3d030107034200") + QByteArray(65, '\x04')).toBase64());

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
    void tlsPasskeySetupUsesTheAuthenticatedSession();
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
    void tlsUpdateDownloadChecksPinAndHash();
    void tlsWrongPinSendsNothing();
    void tlsRateLimitOnBearerCall();
    void tlsRequiresTls13();
    void tlsConnectReturnsDirectRoute();
    void oneShotRequestsSurviveHostClosingEachConnection();
    void remoteDisplaySetupPersistsPerHost();
    void brokerSessionStoreEncodesOnlyUserAndToken();
    void remoteDisplaySetupRejectsInvalidEntries();
    void remoteDisplaySetupSuggestsFittingMode();
    void remoteDisplaySetupMatchesOddScreens();
    void remoteDisplaySetupDialogAndSessionAgree();
    void remoteDisplaySetupMigratesSavedMatchClient();
    void remoteDisplaySetupFiltersModesByHostFlags();
    // Display profiles (per monitor set)
    void displayProfileRoundTrips();
    void displayProfileRejectsInvalidEntries();
    void displayProfileMigratesIdempotently();
    void displayProfileResolutionOrder();
    void displayProfileListsAndForgetsSavedSets();

    // Keepalive
    void keepaliveCadence();
    void keepaliveRetriesThenGivesUp();

    // Passkey (Touch ID) sign-in, section 13.3 / 13.4
    void parsesPasskeyChallenge();
    void rejectsMalformedPasskeyChallenges();
    void passwordAnswersNeverSatisfyPasskeyPrompt();
    void validatesPasskeyRpIdsAndUsernames();
    void buildsPasskeyHelperInput();
    void validatesPasskeyAssertion();
    void tlsPasskeyStartAndRespond();
    void tlsPasswordStartIsUnchanged();
    void tlsPasskeyFallsBackWithoutLocalKey();
    void tlsPasskeyFallsBackOnDenial();
    void tlsPasskeyFallsBackOnStartDenial();
    void tlsPasskeyFallsBackWithoutPasskeyPrompt();
    void tlsPasskeyRefusesOtherRelyingParty();
    void tlsPasskeyNotConfirmed();
    void tlsPasskeyRateLimitThrows();
    void helperExitCodesMapToOutcomes();
    void helperListAndCreateParsing();
    void realHelperWithoutKeyFallsBack();

    // Device-bound sessions (section 14)
    void deviceProofMessageFormat();
    void validatesDeviceKeysAndSignatures();
    void parsesDeviceBound();
    void tlsStartSendsDeviceKeyForBothMethods();
    void tlsStartOmitsUnusableDeviceKey();
    void tlsPasskeySignInSendsDeviceKey();
    void tlsBearerCallsCarryDeviceProof();
    void tlsBearerCallsWithoutSignerAreUnchanged();
    void tlsDeviceSigningFailureSendsNoProof();
    void tlsDeviceBoundSessionRejectionSignsOut();
    void helperDeviceKeyCommands();
    void realHelperDeviceKeyWithoutKey();

    // First sign-in wizard (section 16)
    void qrEncoderMatchesReferenceVectors();
    void qrEncoderFormatInformationRoundTrip();
    void qrEncoderCapacityLimits();
    void parsesEnrollmentReplies();
    void enrollmentUnknownOrMisplacedStatesAreDenied();
    void rejectsMalformedEnrollmentReplies();
    void validatesEnrollmentValues();
    void checksNewPasswordsLocally();
    void enrollmentTextsAreGeneric();
    void enrollmentWalkExpiredPasswordToTouchId();
    void enrollmentWalkEnrolledThenNextCode();
    void enrollmentWalkDeniedAndAlreadyEnrolled();
    void enrollmentWalkDeniedAfterPasswordChange();
    void enrollmentWalkPasskeyRejected();
    void enrollmentWalkTransportErrorsKeepTheStep();
    void enrollmentWalkLostPasswordReply();
    void enrollmentStartSendsDeviceKey();
    void onboardingDecisionPrecedence();
    void onboardingReadsSettingsAndPasskeys();
    // Remote stream settings (per workstation, remote access defaults)
    void remoteStreamSetupPersistsPerHost();
    void remoteStreamSetupAcceptsNvenc420();
    void remoteStreamSetupRejectsInvalidEntries();
    void remoteStreamSetupPicksBitrateForRoute();
    void remoteStreamSetupLayersDefaults();
    void remoteStreamSetupSeedsOnlyFromExactBookmark();
    void remoteStreamSetupReportsUnusableChoice();
    void remoteStreamSetupCachesCapabilities();
    void remoteStreamSetupCachesDisplayCapabilities();
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
    QVERIFY(!reply.passkeySetupAvailable);
    const auto offered = PlankBroker::parseAuthReply(200, json(R"({"state":"authenticated",
        "session_token":"AbC-_123","expires_in":36000,"username":"anna","passkey_setup_available":true})"));
    QVERIFY(offered.passkeySetupAvailable);
}

void TestPlankBroker::tlsPasskeySetupUsesTheAuthenticatedSession()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", R"({"state":"passkey_added"})");
    server.queue("200 OK", R"({"state":"done"})");
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const QString mapping = QStringLiteral("passkey:%1,%2").arg(PasskeyCredential, DeviceSpki);
    client.setupPasskey(QStringLiteral("session-token"), mapping);
    client.skipPasskeySetup(QStringLiteral("session-token"));
    QCOMPARE(server.requestLog.size(), 2);
    QVERIFY(server.requestLog.at(0).startsWith("POST /v1/passkeys/setup HTTP/1.1\r\n"));
    QVERIFY(server.requestLog.at(0).contains("Authorization: Bearer session-token\r\n"));
    QVERIFY(server.requestLog.at(0).contains(mapping.toUtf8()));
    QVERIFY(server.requestLog.at(1).startsWith("POST /v1/passkeys/setup/skip HTTP/1.1\r\n"));
    QVERIFY(server.requestLog.at(1).contains("Authorization: Bearer session-token\r\n"));
    QVERIFY(!server.requestLog.at(1).contains(mapping.toUtf8()));
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
    QCOMPARE(PlankBroker::classifyBearerStatus(409), BearerStatus::Unavailable);
    QCOMPARE(PlankBroker::classifyBearerStatus(302), BearerStatus::Malformed);
    QCOMPARE(PlankBroker::classifyBearerStatus(500), BearerStatus::Malformed);
    QVERIFY(PlankBroker::isTransientOfflineResponse(409,
        R"({"state":"unavailable","reason":"offline"})"));
    QVERIFY(!PlankBroker::isTransientOfflineResponse(409,
        R"({"state":"unavailable","reason":"certificate changed"})"));
    QVERIFY(!PlankBroker::isTransientOfflineResponse(401,
        R"({"state":"unavailable","reason":"offline"})"));
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
    // Real hosts report the digest in upper-case hex (Sunshine util::hex_vec).
    QVERIFY(PlankBroker::resolveTransportTarget(HostPin, 29042, 28989, QString(HostPin).toUpper(), target));
    QCOMPARE(target.port, quint16(29042));
    QCOMPARE(target.certificateSha256, HostPin);
}

void TestPlankBroker::brokeredTransportRequiresPinnedCertificate()
{
    PlankBroker::TransportTarget target;
    const QString other = QString(HostPin).replace(0, 1, QStringLiteral("f"));
    QVERIFY(!PlankBroker::resolveTransportTarget(HostPin, 29042, 28989, other, target));
    QCOMPARE(target.port, quint16(0));
    QVERIFY(!PlankBroker::resolveTransportTarget(HostPin, 29042, 28989, other.toUpper(), target));
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
    // Launch schema 3: services carry the negotiated clipboard flag.
    QJsonObject reply {
        {"schema_version", 3}, {"state", "connecting"},
        {"transport_token", QString::fromLatin1(QByteArray(32, 'k').toBase64())},
        {"udp_port", 28989}, {"max_udp_payload_size", 1200},
        {"capture", topology.toJson().value("capture")},
        {"services", QJsonObject {{"audio", true}, {"input", true}, {"pen", "normalized"},
                                  {"cursor", "embedded"}, {"clipboard", false}}},
    };
    MacPreviewLaunch::Reply parsed;
    // A schema-2 reply (no clipboard flag) is refused, never read as "clipboard off".
    QJsonObject schema2 = reply;
    schema2["schema_version"] = 2;
    schema2["services"] = QJsonObject {{"audio", true}, {"input", true}, {"pen", "normalized"},
                                       {"cursor", "embedded"}};
    QVERIFY(!MacPreviewLaunch::parseReply(schema2, topology, 28989, 1200, parsed));
    // The clipboard flag is only accepted where this platform implements clipboard sync.
    QJsonObject withClipboard = reply;
    withClipboard["services"] = QJsonObject {{"audio", true}, {"input", true}, {"pen", "normalized"},
                                             {"cursor", "embedded"}, {"clipboard", true}};
    QCOMPARE(MacPreviewLaunch::parseReply(withClipboard, topology, 28989, 1200, parsed),
             NvOutputTopology::PlatformClipboardSyncFeature != 0);
    if (NvOutputTopology::PlatformClipboardSyncFeature != 0) QVERIFY(parsed.clipboard);
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

void TestPlankBroker::tlsUpdateDownloadChecksPinAndHash()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = QByteArrayLiteral("not-a-real-dmg");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QStringLiteral("/v1/client-updates/macos-arm64/1.0.139.dmg");
    const QByteArray digest = QCryptographicHash::hash(server.body, QCryptographicHash::Sha256).toHex();
    const QString output = dir.filePath(QStringLiteral("release.dmg"));
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    client.download(path, output, server.body.size(), digest);
    QFile file(output);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), server.body);
    QVERIFY(server.requestLog.last().startsWith("GET /v1/client-updates/macos-arm64/1.0.139.dmg HTTP/1.1\r\n"));

    QVERIFY(QFile::remove(output));
    try {
        client.download(path, output, server.body.size(), QByteArray(64, '0'));
        QFAIL("A mismatched release hash must be rejected");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::Protocol);
    }
    QVERIFY(!QFile::exists(output));

    server.request.clear();
    try {
        PlankBrokerClient(localConfig(server.port(), {QString::fromLatin1(RsaSpkiSha256)}))
                .download(path, output, server.body.size(), digest);
        QFAIL("An unpinned release server must be rejected");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::Tls);
    }
    QVERIFY(server.request.isEmpty());
    QVERIFY(!QFile::exists(output));
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

void TestPlankBroker::tlsConnectReturnsDirectRoute()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = QStringLiteral(R"({"route":"direct","endpoint":"192.168.10.57","port":28989,)"
                                 R"("host_cert_sha256":"%1","username":"anna","gssapi_token":"YWJj",)"
                                 R"("expires_in":30})").arg(HostPin).toUtf8();
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const PlankBroker::Lease lease = client.connect(QStringLiteral("t"), QStringLiteral("ws01.example.test"));
    QCOMPARE(lease.route, PlankBroker::Route::Direct);
    QCOMPARE(lease.endpoint, QStringLiteral("192.168.10.57"));
    QCOMPARE(lease.port, quint16(28989));
    QVERIFY(server.request.startsWith("POST /v1/hosts/ws01.example.test/connect HTTP/1.1\r\n"));
    QVERIFY(server.request.endsWith("{}"));
}

void TestPlankBroker::oneShotRequestsSurviveHostClosingEachConnection()
{
    // The PLANK host closes every TLS connection after its reply without saying so.
    // Back-to-back requests (brokered flow: /serverinfo, then /plank/auth/start a few
    // milliseconds later) must each get their own connection, never a closing one.
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    server.announceClose = false;
    server.body = R"({"state":"ok"})";
    QVERIFY(server.listen());
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    QObject::connect(&manager, &QNetworkAccessManager::sslErrors, &manager,
                     [](QNetworkReply* reply, const QList<QSslError>& errors) { reply->ignoreSslErrors(errors); });
    constexpr int Requests = 10;
    for (int i = 0; i < Requests; ++i) {
        QNetworkRequest request(QUrl(QStringLiteral("https://127.0.0.1:%1/plank/auth/start").arg(server.port())));
        QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_3OrLater);
        request.setSslConfiguration(tls);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        PlankHttp::prepareOneShotRequest(request);
        QScopedPointer<QNetworkReply> reply(i % 2 ? manager.post(request, QByteArray("{}")) : manager.get(request));
        QEventLoop loop;
        QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        if (!reply->isFinished()) loop.exec();
        QVERIFY2(reply->isFinished(), qPrintable(QStringLiteral("request %1 timed out").arg(i)));
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), server.body);
    }
    QCOMPARE(server.requests, Requests);
    QCOMPARE(server.connections, Requests);
    QCOMPARE(server.request.count("Connection: close\r\n"), Requests);
}

namespace {
NvClientDisplay screen(int x, int width, int height)
{
    return NvClientDisplay { QRect(x, 0, width, height), QSize(width, height) };
}
}

void TestPlankBroker::brokerSessionStoreEncodesOnlyUserAndToken()
{
    const QString token = QStringLiteral("aB3-_xYz0123456789aB3-_xYz0123456789aB3-_xY");
    const QByteArray encoded = BrokerSessionStore::encode(QStringLiteral("finn"), token);
    const QJsonObject object = QJsonDocument::fromJson(encoded).object();
    QCOMPARE(object.keys(), QStringList({QStringLiteral("token"), QStringLiteral("user"), QStringLiteral("v")}));
    BrokerSessionStore::Saved saved;
    QVERIFY(BrokerSessionStore::decode(encoded, saved));
    QCOMPARE(saved.username, QStringLiteral("finn"));
    QCOMPARE(saved.token, token);
    // Anything unexpected is treated as "no saved session".
    for (const QByteArray& bad : {QByteArray(), QByteArray("not json"), QByteArray("[]"),
                                  QByteArray(R"({"v":2,"user":"finn","token":"abc"})"),
                                  QByteArray(R"({"v":1,"user":"","token":"abc"})"),
                                  QByteArray(R"({"v":1,"user":"finn","token":""})"),
                                  QByteArray(R"({"v":1,"user":"finn","token":"has space"})"),
                                  QByteArray(R"({"v":1,"user":"finn","token":"é"})"),
                                  QByteArray(5000, 'x')}) {
        QVERIFY2(!BrokerSessionStore::decode(bad, saved), bad.left(60).constData());
        QVERIFY(saved.token.isEmpty());
        QVERIFY(saved.username.isEmpty());
    }
}

void TestPlankBroker::remoteDisplaySetupPersistsPerHost()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    QVERIFY(!RemoteDisplaySetup::load(settings, QStringLiteral("ws01.example.test")).configured);

    RemoteDisplaySetup::Setup setup;
    setup.hostLayout = RemoteDisplaySetup::layoutForChoice(RemoteDisplaySetup::SingleVirtual);
    setup.virtualMode1 = QStringLiteral("2560x1600");
    setup.virtualMode2 = QStringLiteral("1920x1080");
    setup.scalingMode = RemoteDisplaySetup::scalingForChoice(RemoteDisplaySetup::ScaledSpan);
    QVERIFY(RemoteDisplaySetup::save(settings, QStringLiteral("WS01.example.test"), setup));

    const RemoteDisplaySetup::Setup loaded = RemoteDisplaySetup::load(settings, QStringLiteral("ws01.example.test"));
    QVERIFY(loaded.configured);
    QCOMPARE(loaded.hostLayout, QStringLiteral("single"));
    QCOMPARE(loaded.virtualMode1, QStringLiteral("2560x1600"));
    QCOMPARE(loaded.virtualMode2, QStringLiteral("1920x1080"));
    QCOMPARE(loaded.scalingMode, QStringLiteral("scaled-span"));
    QCOMPARE(RemoteDisplaySetup::choiceForLayout(loaded.hostLayout), int(RemoteDisplaySetup::SingleVirtual));
    // Other workstations are independent.
    QVERIFY(!RemoteDisplaySetup::load(settings, QStringLiteral("ws02.example.test")).configured);
}

void TestPlankBroker::remoteDisplaySetupRejectsInvalidEntries()
{
    RemoteDisplaySetup::Setup setup;
    setup.hostLayout = QStringLiteral("single");
    setup.virtualMode1 = QStringLiteral("3024x1964"); // a MacBook panel, not a qualified mode
    setup.virtualMode2 = QStringLiteral("1920x1080");
    setup.scalingMode = QStringLiteral("native");
    QVERIFY(!RemoteDisplaySetup::isValid(setup));
    setup.virtualMode1 = QStringLiteral("2560x1600");
    QVERIFY(RemoteDisplaySetup::isValid(setup));
    setup.hostLayout = QStringLiteral("fixed");       // Mac-only internal layout, never stored
    QVERIFY(!RemoteDisplaySetup::isValid(setup));
    setup.hostLayout = QStringLiteral("physical");
    setup.scalingMode = QStringLiteral("stretch");
    QVERIFY(!RemoteDisplaySetup::isValid(setup));

    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    QVERIFY(!RemoteDisplaySetup::save(settings, QStringLiteral("ws01.example.test"), setup));
    // A hand-edited or outdated entry is asked again rather than used.
    settings.setValue(QStringLiteral("remote-hosts/ws01.example.test/host-layout"), QStringLiteral("single"));
    settings.setValue(QStringLiteral("remote-hosts/ws01.example.test/virtual-mode-1"), QStringLiteral("9999x9999"));
    settings.setValue(QStringLiteral("remote-hosts/ws01.example.test/virtual-mode-2"), QStringLiteral("1920x1080"));
    settings.setValue(QStringLiteral("remote-hosts/ws01.example.test/scaling-mode"), QStringLiteral("native"));
    QVERIFY(!RemoteDisplaySetup::load(settings, QStringLiteral("ws01.example.test")).configured);
}

void TestPlankBroker::remoteDisplaySetupSuggestsFittingMode()
{
    // MacBook Pro 14" (3024x1964) and MacBook Air 13" (2560x1664) panels:
    // each gets its fullscreen viewport below the camera housing.
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(3024, 1964)), QStringLiteral("3024x1890"));
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(2560, 1664)), QStringLiteral("2560x1600"));
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(3840, 2160)), QStringLiteral("3840x2160"));
    // 16:9 5K keeps its shape rather than the wider 5120x2160.
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(5120, 2880)), QStringLiteral("3840x2160"));
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(1920, 1080)), QStringLiteral("1920x1080"));
    // Nothing fits: the closest aspect ratio (16:10) that enlarges least.
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(1440, 900)), QStringLiteral("1920x1200"));
    // A portrait main display still gets a landscape single virtual display.
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(1280, 2160)), QStringLiteral("1920x1080"));
    for (const QSize size : {QSize(3024, 1964), QSize(1920, 1200), QSize(1024, 768), QSize()}) {
        QVERIFY(RemoteDisplaySetup::isQualifiedMode(RemoteDisplaySetup::suggestedMode(size)));
    }
}

void TestPlankBroker::remoteDisplaySetupMatchesOddScreens()
{
    // Odd panels are matched to the closest supported mode, never refused.
    QString reason;
    QVERIFY(RemoteDisplaySetup::canMatchClient({screen(0, 3024, 1964)}, &reason));
    QVERIFY(RemoteDisplaySetup::canMatchClient({screen(0, 2560, 1440)}));
    QVERIFY(RemoteDisplaySetup::canMatchClient({screen(0, 3840, 2160), screen(3840, 2560, 1440)}));
    // Only the arrangement can rule matching out.
    QVERIFY(!RemoteDisplaySetup::canMatchClient({screen(0, 1920, 1080), screen(1920, 1920, 1080),
                                                 screen(3840, 1920, 1080)}, &reason));
    QVERIFY(reason.contains(QStringLiteral("one or two")));

    // A 14" MacBook Pro panel: its 16:10 fullscreen viewport below the notch
    // is the largest qualified mode that fits without upscaling.
    RemoteDisplaySetup::Setup proposal = RemoteDisplaySetup::proposal({screen(0, 3024, 1964)});
    QCOMPARE(proposal.hostLayout, QStringLiteral("match-client"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("3024x1890"));
    QCOMPARE(proposal.scalingMode, QStringLiteral("scaled-span"));
    QVERIFY(RemoteDisplaySetup::isValid(proposal));
    proposal = RemoteDisplaySetup::proposal({screen(0, 3840, 2160), screen(3840, 2560, 1440)});
    QCOMPARE(proposal.hostLayout, QStringLiteral("match-client"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("3840x2160"));
    QCOMPARE(proposal.virtualMode2, QStringLiteral("2560x1440"));
    // When matching cannot work, one virtual display that fits the main one.
    proposal = RemoteDisplaySetup::proposal({screen(0, 3024, 1964), screen(3024, 1920, 1080),
                                             screen(4944, 1920, 1080)});
    QCOMPARE(proposal.hostLayout, QStringLiteral("single"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("3024x1890"));
    QVERIFY(RemoteDisplaySetup::isValid(proposal));
}

void TestPlankBroker::remoteDisplaySetupDialogAndSessionAgree()
{
    // A MacBook Pro 14" in the 1x "1920x1200" desktop mode, as ClientDisplayProbe
    // reports it: logical 1920x1200 points, panel 3024x1964, backing 1920x1200.
    const NvClientDisplay oneX {QRect(0, 0, 1920, 1200), QSize(3024, 1964), QSize(1920, 1200)};
    QCOMPARE(ClientDisplayProbe::describe(oneX), QStringLiteral("3024 × 1964 display, desktop 1920 × 1200 (1×)"));
    const ClientDisplayProbe::MatchPreview dialog = ClientDisplayProbe::matchPreview({oneX});
    QVERIFY(dialog.ok);
    QCOMPARE(dialog.modes, QStringList({QStringLiteral("1920x1200")}));
    QVERIFY(!dialog.fitted);
    QCOMPARE(ClientDisplayProbe::matchSummary(dialog), QStringLiteral("1920 × 1200 (exact)"));
    const RemoteDisplaySetup::Setup proposal = RemoteDisplaySetup::proposal({oneX});
    QCOMPARE(proposal.hostLayout, QStringLiteral("match-client"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("1920x1200"));

    // The Session maps each SDL display (logical bounds, SDL native mode = the
    // panel) through ClientDisplayProbe::forSessionDisplay against the same
    // probe list and resolves with the result. It must not fall back to the
    // panel as the desktop: that was the 3024x1964 -> 2560x1600 regression.
    const QVector<NvClientDisplay> probed {oneX};
    const NvClientDisplay session = ClientDisplayProbe::forSessionDisplay(
                QRect(0, 0, 1920, 1200), QSize(3024, 1964), probed);
    QCOMPARE(session.bounds, oneX.bounds);
    QCOMPARE(session.nativeSize, QSize(3024, 1964));
    QCOMPARE(session.backingSize, QSize(1920, 1200));
    QString layout;
    QStringList modes;
    bool fitted = true;
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout({session}, layout, modes, nullptr, &fitted));
    QCOMPARE(layout, dialog.hostLayout);
    QCOMPARE(modes, dialog.modes);
    QCOMPARE(fitted, dialog.fitted);
    QCOMPARE(NvOutputTopology::clientMatchTarget(session), NvOutputTopology::clientMatchTarget(oneX));
    QCOMPARE(NvOutputTopology::clientMatchTarget(session), QSize(1920, 1200));
    QCOMPARE(ClientDisplayProbe::logLine(session, modes.first(), !fitted),
             QStringLiteral("PLANK client display: panel=3024x1964 desktop=1920x1200@1 target=1920x1200 match=1920x1200 (exact)"));

    // Two displays in any SDL order: each finds its own probe entry by bounds,
    // and the Session's list resolves exactly like the dialog's.
    const NvClientDisplay external {QRect(1920, 0, 2560, 1440), QSize(2560, 1440), QSize(2560, 1440)};
    const QVector<NvClientDisplay> probedPair {oneX, external};
    const ClientDisplayProbe::MatchPreview pairDialog = ClientDisplayProbe::matchPreview(probedPair);
    QVERIFY(pairDialog.ok);
    QVector<NvClientDisplay> sessionPair {
        ClientDisplayProbe::forSessionDisplay(QRect(1920, 0, 2560, 1440), QSize(2560, 1440), probedPair),
        ClientDisplayProbe::forSessionDisplay(QRect(0, 0, 1920, 1200), QSize(3024, 1964), probedPair)};
    QCOMPARE(sessionPair.at(0).backingSize, external.backingSize);
    QCOMPARE(sessionPair.at(1).backingSize, oneX.backingSize);
    std::swap(sessionPair[0], sessionPair[1]);
    QVERIFY(NvOutputTopology::resolveClientDisplayLayout(sessionPair, layout, modes, nullptr, &fitted));
    QCOMPARE(layout, pairDialog.hostLayout);
    QCOMPARE(modes, pairDialog.modes);
    QCOMPARE(fitted, pairDialog.fitted);

    // No probe entry for these bounds (no probe on this platform, or the
    // display moved): the SDL native size is the panel, the desktop unknown.
    const NvClientDisplay unprobed = ClientDisplayProbe::forSessionDisplay(
                QRect(0, 0, 1512, 982), QSize(3024, 1964), probed);
    QCOMPARE(unprobed.bounds, QRect(0, 0, 1512, 982));
    QCOMPARE(unprobed.nativeSize, QSize(3024, 1964));
    QVERIFY(!unprobed.backingSize.isValid());
    QCOMPARE(NvOutputTopology::clientMatchTarget(unprobed), QSize(3024, 1964));

    // Default Retina (1512x982 points @2x) and "More Space" (1800x1169 @2x)
    // without a fullscreen viewport: both aim at the 3024x1964 panel and get
    // the largest 16:10 mode that fits, 3024x1890, letterboxed.
    for (const NvClientDisplay& retina : {NvClientDisplay {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964)},
                                          NvClientDisplay {QRect(0, 0, 1800, 1169), QSize(3024, 1964), QSize(3600, 2338)}}) {
        const ClientDisplayProbe::MatchPreview preview = ClientDisplayProbe::matchPreview({retina});
        QVERIFY(preview.ok);
        QVERIFY(preview.fitted);
        QCOMPARE(preview.modes, QStringList({QStringLiteral("3024x1890")}));
        QCOMPARE(ClientDisplayProbe::matchSummary(preview), QStringLiteral("3024 × 1890 (closest supported size)"));
        QVERIFY(ClientDisplayProbe::describe(retina).contains(QStringLiteral("(2×)")));
        QVERIFY(ClientDisplayProbe::logLine(retina, preview.modes.first(), false).endsWith(
                    QStringLiteral("target=3024x1964 match=3024x1890 (fitted)")));
    }
    // Notched 14" at the default 1512x982 pt: native fullscreen is the 3024x1890 viewport below the camera
    // housing. The Session inherits it from the probe, so both aim at the 16:10 viewport, not the panel.
    const NvClientDisplay notched {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1890)};
    const QVector<NvClientDisplay> probedNotched {notched};
    const NvClientDisplay notchedSession = ClientDisplayProbe::forSessionDisplay(
                QRect(0, 0, 1512, 982), QSize(3024, 1964), probedNotched);
    QCOMPARE(notchedSession.fullscreenSize, QSize(3024, 1890));
    QCOMPARE(NvOutputTopology::clientMatchTarget(notchedSession), QSize(3024, 1890));
    // That viewport is a qualified mode, so it is matched 1:1.
    const ClientDisplayProbe::MatchPreview notchedPreview = ClientDisplayProbe::matchPreview(probedNotched);
    QCOMPARE(notchedPreview.modes, QStringList({QStringLiteral("3024x1890")}));
    QVERIFY(!notchedPreview.fitted);
    QCOMPARE(ClientDisplayProbe::matchSummary(notchedPreview), QStringLiteral("3024 × 1890 (exact)"));
    QVERIFY(ClientDisplayProbe::logLine(notchedSession, notchedPreview.modes.first(), true).endsWith(
                QStringLiteral("target=3024x1890 match=3024x1890 (exact)")));
    // A plain external monitor describes as its size.
    QCOMPARE(ClientDisplayProbe::describe({screen(0, 2560, 1440), screen(2560, 1920, 1080)}),
             QStringLiteral("2560 × 1440 + 1920 × 1080"));
}

void TestPlankBroker::remoteDisplaySetupMigratesSavedMatchClient()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    // What 1.0.128 stored on a MacBook in the 1x mode: Match with the Qt-based
    // 1920x1200 guess. Host ids are case-insensitive keys.
    settings.setValue(QStringLiteral("remote-hosts/studio-a.example.test/host-layout"), QStringLiteral("match-client"));
    settings.setValue(QStringLiteral("remote-hosts/studio-a.example.test/virtual-mode-1"), QStringLiteral("1920x1200"));
    settings.setValue(QStringLiteral("remote-hosts/studio-a.example.test/virtual-mode-2"), QStringLiteral("1920x1200"));
    settings.setValue(QStringLiteral("remote-hosts/studio-a.example.test/scaling-mode"), QStringLiteral("scaled-span"));
    RemoteDisplaySetup::Setup setup = RemoteDisplaySetup::load(settings, QStringLiteral("Studio-A.Example.Test"));
    QVERIFY(setup.configured);

    // Connect on the Retina default: the modes are re-resolved, not reused.
    const NvClientDisplay retina {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964)};
    QString reason;
    QVERIFY(RemoteDisplaySetup::refreshMatchedModes(settings, QStringLiteral("studio-a.example.test"), setup,
                                                    {retina}, &reason));
    QCOMPARE(setup.virtualMode1, QStringLiteral("3024x1890"));
    QCOMPARE(setup.virtualMode2, QStringLiteral("3024x1890"));
    setup = RemoteDisplaySetup::load(settings, QStringLiteral("studio-a.example.test"));
    QVERIFY(setup.configured);
    QCOMPARE(setup.hostLayout, QStringLiteral("match-client"));
    QCOMPARE(setup.virtualMode1, QStringLiteral("3024x1890"));

    // A stored mode off the allowlist does not force the dialog for Match.
    settings.setValue(QStringLiteral("remote-hosts/studio-b.example.test/host-layout"), QStringLiteral("match-client"));
    settings.setValue(QStringLiteral("remote-hosts/studio-b.example.test/virtual-mode-1"), QStringLiteral("3024x1964"));
    settings.setValue(QStringLiteral("remote-hosts/studio-b.example.test/scaling-mode"), QStringLiteral("native"));
    setup = RemoteDisplaySetup::load(settings, QStringLiteral("studio-b.example.test"));
    QVERIFY(setup.configured);
    QVERIFY(RemoteDisplaySetup::refreshMatchedModes(settings, QStringLiteral("studio-b.example.test"), setup, {retina}));
    QCOMPARE(RemoteDisplaySetup::load(settings, QStringLiteral("studio-b.example.test")).virtualMode1,
             QStringLiteral("3024x1890"));
    // ...but a single virtual display with such a mode is still asked again.
    settings.setValue(QStringLiteral("remote-hosts/studio-b.example.test/host-layout"), QStringLiteral("single"));
    settings.setValue(QStringLiteral("remote-hosts/studio-b.example.test/virtual-mode-1"), QStringLiteral("3024x1964"));
    QVERIFY(!RemoteDisplaySetup::load(settings, QStringLiteral("studio-b.example.test")).configured);

    // Other layouts are left alone; an unmatchable arrangement reports why.
    setup = RemoteDisplaySetup::Setup {};
    setup.hostLayout = QStringLiteral("single");
    setup.virtualMode1 = setup.virtualMode2 = QStringLiteral("1920x1080");
    QVERIFY(RemoteDisplaySetup::refreshMatchedModes(settings, QStringLiteral("studio-c.example.test"), setup, {}));
    QCOMPARE(setup.virtualMode1, QStringLiteral("1920x1080"));
    setup.hostLayout = QStringLiteral("match-client");
    QVERIFY(!RemoteDisplaySetup::refreshMatchedModes(settings, QStringLiteral("studio-c.example.test"), setup,
                                                     {screen(0, 1920, 1080), NvClientDisplay {QRect(0, 1080, 1920, 1080), QSize(1920, 1080)}},
                                                     &reason));
    QVERIFY(reason.contains(QStringLiteral("left to right")));
}

void TestPlankBroker::remoteDisplaySetupFiltersModesByHostFlags()
{
    // A workstation whose last connect did not advertise the notch-safe
    // laptop modes (0x4000000) refuses 3024x1890: nothing may propose it.
    const QStringList oldHost = RemoteDisplaySetup::candidateModes(true, 0);
    const QStringList newHost = RemoteDisplaySetup::candidateModes(
                true, NvOutputTopology::NotchSafeLaptopModesFeature);
    QVERIFY(!oldHost.contains(QStringLiteral("3024x1890")));
    QVERIFY(newHost.contains(QStringLiteral("3024x1890")));
    // Nothing known yet: every qualified mode (the Session re-resolves with
    // the flags of the connect itself).
    QCOMPARE(RemoteDisplaySetup::candidateModes(false, 0), NvOutputTopology::qualifiedVirtualModes());

    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(3024, 1964), oldHost), QStringLiteral("2560x1600"));
    QCOMPARE(RemoteDisplaySetup::suggestedMode(QSize(3024, 1964), newHost), QStringLiteral("3024x1890"));

    const NvClientDisplay notched {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1890)};
    QString reason;
    QVERIFY(RemoteDisplaySetup::canMatchClient({notched}, &reason, oldHost));
    RemoteDisplaySetup::Setup proposal = RemoteDisplaySetup::proposal({notched}, oldHost);
    QCOMPARE(proposal.hostLayout, QStringLiteral("match-client"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("2560x1600"));
    proposal = RemoteDisplaySetup::proposal({notched}, newHost);
    QCOMPARE(proposal.virtualMode1, QStringLiteral("3024x1890"));
    // The unmatchable fallback (three monitors) is filtered as well.
    proposal = RemoteDisplaySetup::proposal({notched, screen(1512, 1920, 1080), screen(3432, 1920, 1080)}, oldHost);
    QCOMPARE(proposal.hostLayout, QStringLiteral("single"));
    QCOMPARE(proposal.virtualMode1, QStringLiteral("2560x1600"));

    const ClientDisplayProbe::MatchPreview preview = ClientDisplayProbe::matchPreview({notched}, oldHost);
    QVERIFY(preview.ok);
    QVERIFY(preview.fitted);
    QCOMPARE(preview.modes, QStringList({QStringLiteral("2560x1600")}));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    RemoteDisplaySetup::Setup setup;
    setup.hostLayout = QStringLiteral("match-client");
    setup.virtualMode1 = setup.virtualMode2 = QStringLiteral("3024x1890");
    setup.scalingMode = QStringLiteral("scaled-span");
    QVERIFY(RemoteDisplaySetup::save(settings, QStringLiteral("ws01.example.test"), setup));
    QVERIFY(RemoteDisplaySetup::refreshMatchedModes(settings, QStringLiteral("ws01.example.test"), setup,
                                                    {notched}, &reason, oldHost));
    QCOMPARE(RemoteDisplaySetup::load(settings, QStringLiteral("ws01.example.test")).virtualMode1,
             QStringLiteral("2560x1600"));
    QVERIFY(RemoteDisplaySetup::unsupportedModeReason(setup, oldHost).isEmpty());

    // A saved single virtual 3024x1890 is asked again for the old host only.
    setup.hostLayout = QStringLiteral("single");
    setup.virtualMode1 = QStringLiteral("3024x1890");
    QVERIFY(RemoteDisplaySetup::unsupportedModeReason(setup, oldHost).contains(QStringLiteral("3024")));
    QVERIFY(RemoteDisplaySetup::unsupportedModeReason(setup, newHost).isEmpty());
    setup.hostLayout = QStringLiteral("dual-horizontal");
    setup.virtualMode1 = QStringLiteral("1920x1080");
    setup.virtualMode2 = QStringLiteral("3024x1890");
    QVERIFY(!RemoteDisplaySetup::unsupportedModeReason(setup, oldHost).isEmpty());
    // Physical uses no virtual mode at all.
    setup.hostLayout = QStringLiteral("physical");
    QVERIFY(RemoteDisplaySetup::unsupportedModeReason(setup, oldHost).isEmpty());
}

void TestPlankBroker::displayProfileRoundTrips()
{
    DisplayProfile::Profile profile;
    profile.primary = QStringLiteral("uuid:B");
    profile.presentation = QStringLiteral("single");
    profile.scaling = QStringLiteral("fit");
    profile.manual = true;
    DisplayProfile::MonitorChoice& laptop = profile.choice(QStringLiteral("uuid:A"));
    laptop.size = DisplayProfile::SizeMode::LooksLike;
    laptop.on = false;
    DisplayProfile::MonitorChoice& external = profile.choice(QStringLiteral("uuid:B"));
    QVERIFY(DisplayProfile::sizeFromText(QStringLiteral("custom:5120x1440"), external));
    external.hasPosition = true;
    external.position = QPoint(1512, -200);
    external.backing = QStringLiteral("virtual");
    QCOMPARE(profile.monitors.size(), 2);
    QCOMPARE(&profile.choice(QStringLiteral("uuid:A")), &profile.monitors[0]);

    const QString encoded = DisplayProfile::encode(profile);
    DisplayProfile::Profile decoded;
    QVERIFY(DisplayProfile::decode(encoded, decoded));
    QCOMPARE(DisplayProfile::encode(decoded), encoded);
    QCOMPARE(decoded.primary, QStringLiteral("uuid:B"));
    QCOMPARE(decoded.presentation, QStringLiteral("single"));
    QCOMPARE(decoded.scaling, QStringLiteral("fit"));
    QVERIFY(decoded.manual);
    QVERIFY(!decoded.find(QStringLiteral("uuid:A"))->on);
    QCOMPARE(decoded.find(QStringLiteral("uuid:A"))->size, DisplayProfile::SizeMode::LooksLike);
    QCOMPARE(decoded.find(QStringLiteral("uuid:B"))->size, DisplayProfile::SizeMode::Custom);
    QCOMPARE(decoded.find(QStringLiteral("uuid:B"))->fixedSize, QSize(5120, 1440));
    QCOMPARE(decoded.find(QStringLiteral("uuid:B"))->position, QPoint(1512, -200));
    QCOMPARE(decoded.find(QStringLiteral("uuid:B"))->backing, QStringLiteral("virtual"));
    QVERIFY(!decoded.find(QStringLiteral("uuid:A"))->hasPosition);
    QVERIFY(decoded.find(QStringLiteral("uuid:C")) == nullptr);

    DisplayProfile::MonitorChoice preset;
    QVERIFY(DisplayProfile::sizeFromText(QStringLiteral("preset:3840x2160"), preset));
    QCOMPARE(DisplayProfile::sizeText(preset), QStringLiteral("preset:3840x2160"));
}

void TestPlankBroker::displayProfileRejectsInvalidEntries()
{
    DisplayProfile::MonitorChoice monitor;
    for (const char* bad : {"custom:3841x2160", "custom:03840x2160", "custom:x2160", "preset:3840", "large",
                            "custom:0x0", "custom:20000x2160", ""}) {
        QVERIFY2(!DisplayProfile::sizeFromText(QString::fromLatin1(bad), monitor), bad);
    }
    DisplayProfile::Profile profile;
    for (const char* bad : {R"({"v":2,"monitors":[]})", R"({"v":1})",
                            R"({"v":1,"monitors":[{"key":""}]})",
                            R"({"v":1,"monitors":[{"key":"a"},{"key":"a"}]})",
                            R"({"v":1,"monitors":[{"key":"a","size":"huge"}]})",
                            R"({"v":1,"monitors":[{"key":"a","backing":"dongle"}]})",
                            R"({"v":1,"monitors":[{"key":"a","pos":"left"}]})",
                            R"({"v":1,"monitors":[],"presentation":"tiles"})",
                            R"({"v":1,"monitors":[],"scaling":"stretch"})",
                            "not json"}) {
        QVERIFY2(!DisplayProfile::decode(QString::fromLatin1(bad), profile), bad);
    }
    QVERIFY(DisplayProfile::decode(QStringLiteral(R"({"v":1,"monitors":[{"key":"a"}]})"), profile));
    QCOMPARE(profile.monitors.first().size, DisplayProfile::SizeMode::Exact);
    QVERIFY(profile.monitors.first().on);
    QCOMPARE(profile.presentation, QStringLiteral("windows"));
    // Fingerprints are lowercase hex only: nothing else reaches a settings key.
    QVERIFY(!DisplayProfile::validFingerprint(QStringLiteral("../x")));
    QVERIFY(!DisplayProfile::validFingerprint(QStringLiteral("ABC")));
    QVERIFY(DisplayProfile::validFingerprint(QStringLiteral("0123456789abcdef0123")));
}

void TestPlankBroker::displayProfileMigratesIdempotently()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const auto oldLayout = [&settings](const char* host, const char* layout) {
        const QString group = QStringLiteral("remote-hosts/") + QString::fromLatin1(host);
        settings.setValue(group + QStringLiteral("/host-layout"), QString::fromLatin1(layout));
        settings.setValue(group + QStringLiteral("/virtual-mode-1"), QStringLiteral("1920x1080"));
        settings.setValue(group + QStringLiteral("/virtual-mode-2"), QStringLiteral("1920x1080"));
        settings.setValue(group + QStringLiteral("/scaling-mode"), QStringLiteral("scaled-span"));
    };
    oldLayout("match.example.test", "match-client");
    oldLayout("physical.example.test", "physical");
    oldLayout("single.example.test", "single");
    oldLayout("dual.example.test", "dual-horizontal");
    // Stream settings alone: no display layout to migrate.
    settings.setValue(QStringLiteral("remote-hosts/stream.example.test/stream/mode"), QStringLiteral("defaults"));

    const QString fp = QStringLiteral("00112233445566778899");
    DisplayProfile::Profile proposal;
    proposal.choice(QStringLiteral("uuid:A"));
    QCOMPARE(DisplayProfile::migrate(settings, fp, QStringLiteral("Built-in"), proposal), 4);
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("MATCH.example.test")), DisplayProfile::HostMode::Follow);
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("physical.example.test")), DisplayProfile::HostMode::Legacy);
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("single.example.test")), DisplayProfile::HostMode::Legacy);
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("dual.example.test")), DisplayProfile::HostMode::Legacy);
    QVERIFY(!DisplayProfile::hostModeSaved(settings, QStringLiteral("stream.example.test")));
    // Match users get the proposal for the current monitor set and the banner once.
    DisplayProfile::Profile saved;
    QVERIFY(DisplayProfile::loadGlobal(settings, fp, saved));
    QCOMPARE(DisplayProfile::encode(saved), DisplayProfile::encode(proposal));
    QVERIFY(DisplayProfile::chooseBannerPending(settings));
    // The old keys are never deleted: legacy workstations keep using them.
    for (const char* host : {"match.example.test", "physical.example.test", "single.example.test", "dual.example.test"}) {
        QVERIFY(RemoteDisplaySetup::load(settings, QString::fromLatin1(host)).configured);
    }
    QCOMPARE(RemoteDisplaySetup::load(settings, QStringLiteral("physical.example.test")).hostLayout,
             QStringLiteral("physical"));

    // Idempotent: nothing is migrated twice, a dismissed banner stays dismissed,
    // and a later user choice is not overwritten.
    DisplayProfile::clearChooseBanner(settings);
    DisplayProfile::setHostMode(settings, QStringLiteral("physical.example.test"), DisplayProfile::HostMode::Follow);
    DisplayProfile::Profile other;
    other.choice(QStringLiteral("uuid:Z"));
    QCOMPARE(DisplayProfile::migrate(settings, fp, QStringLiteral("Built-in"), other), 0);
    QVERIFY(!DisplayProfile::chooseBannerPending(settings));
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("physical.example.test")), DisplayProfile::HostMode::Follow);
    QVERIFY(DisplayProfile::loadGlobal(settings, fp, saved));
    QCOMPARE(DisplayProfile::encode(saved), DisplayProfile::encode(proposal));
    // A workstation first seen after the migration is migrated on its own.
    oldLayout("late.example.test", "single");
    QCOMPARE(DisplayProfile::migrate(settings, fp, QStringLiteral("Built-in"), other), 1);
    QCOMPARE(DisplayProfile::hostMode(settings, QStringLiteral("late.example.test")), DisplayProfile::HostMode::Legacy);
}

void TestPlankBroker::displayProfileResolutionOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const QString fp = QStringLiteral("aaaaaaaaaaaaaaaaaaaa");
    const QString host = QStringLiteral("WS01.example.test");
    // Nothing saved: a new monitor set.
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, fp).source, DisplayProfile::Resolved::None);
    QCOMPARE(DisplayProfile::hostMode(settings, host), DisplayProfile::HostMode::Follow);

    DisplayProfile::Profile global;
    global.choice(QStringLiteral("uuid:A"));
    DisplayProfile::saveGlobal(settings, fp, QStringLiteral("Built-in"), global);
    DisplayProfile::Resolved resolved = DisplayProfile::resolveForHost(settings, host, fp);
    QCOMPARE(resolved.source, DisplayProfile::Resolved::Global);
    QCOMPARE(DisplayProfile::encode(resolved.profile), DisplayProfile::encode(global));
    // Another monitor set is still new.
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, QStringLiteral("bbbbbbbbbbbbbbbbbbbb")).source,
             DisplayProfile::Resolved::None);

    // Custom for this workstation, on this monitor set only.
    DisplayProfile::Profile custom;
    custom.choice(QStringLiteral("uuid:A")).size = DisplayProfile::SizeMode::LooksLike;
    DisplayProfile::setHostMode(settings, host, DisplayProfile::HostMode::Custom);
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, fp).source, DisplayProfile::Resolved::Global);
    DisplayProfile::saveHost(settings, host, fp, custom);
    resolved = DisplayProfile::resolveForHost(settings, host.toLower(), fp);
    QCOMPARE(resolved.source, DisplayProfile::Resolved::HostCustom);
    QCOMPARE(DisplayProfile::encode(resolved.profile), DisplayProfile::encode(custom));
    // Other workstations follow the global layout.
    QCOMPARE(DisplayProfile::resolveForHost(settings, QStringLiteral("ws02.example.test"), fp).source,
             DisplayProfile::Resolved::Global);
    // Following again ignores (but keeps) the custom layout.
    DisplayProfile::setHostMode(settings, host, DisplayProfile::HostMode::Follow);
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, fp).source, DisplayProfile::Resolved::Global);
    DisplayProfile::Profile kept;
    QVERIFY(DisplayProfile::loadHost(settings, host, fp, kept));
    // Legacy wins over everything: the pre-profile keys decide.
    DisplayProfile::setHostMode(settings, host, DisplayProfile::HostMode::Legacy);
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, fp).source, DisplayProfile::Resolved::Legacy);
    // No workstation (a LAN bookmark): the global layout.
    QCOMPARE(DisplayProfile::resolveForHost(settings, QString(), fp).source, DisplayProfile::Resolved::Global);
    // Forgetting the custom layout falls back to the global one.
    DisplayProfile::setHostMode(settings, host, DisplayProfile::HostMode::Custom);
    DisplayProfile::forgetHost(settings, host, fp);
    QCOMPARE(DisplayProfile::resolveForHost(settings, host, fp).source, DisplayProfile::Resolved::Global);

    // Settings defaults: ask when screens change, never accept silently.
    QVERIFY(DisplayProfile::askOnChange(settings));
    QVERIFY(!DisplayProfile::autoAccept(settings));
    DisplayProfile::setAskOnChange(settings, false);
    DisplayProfile::setAutoAccept(settings, true);
    QVERIFY(!DisplayProfile::askOnChange(settings));
    QVERIFY(DisplayProfile::autoAccept(settings));
}

void TestPlankBroker::displayProfileListsAndForgetsSavedSets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    DisplayProfile::Profile profile;
    profile.choice(QStringLiteral("uuid:A"));
    DisplayProfile::saveGlobal(settings, QStringLiteral("aaaaaaaaaaaaaaaaaaaa"), QStringLiteral("Built-in"), profile);
    DisplayProfile::saveGlobal(settings, QStringLiteral("bbbbbbbbbbbbbbbbbbbb"), QStringLiteral("Built-in + LG"), profile);
    // An unreadable entry is not listed.
    settings.setValue(QStringLiteral("display-profiles/sets/cccccccccccccccccccc/profile"), QStringLiteral("{"));
    // Nor is an invalid fingerprint ever written.
    DisplayProfile::saveGlobal(settings, QStringLiteral("../../x"), QStringLiteral("x"), profile);
    QVector<DisplayProfile::SavedSet> sets = DisplayProfile::savedSets(settings);
    QCOMPARE(sets.size(), 2);
    QStringList labels;
    for (const auto& set : sets) labels.append(set.label);
    labels.sort();
    QCOMPARE(labels, QStringList({QStringLiteral("Built-in"), QStringLiteral("Built-in + LG")}));
    DisplayProfile::forgetGlobal(settings, QStringLiteral("aaaaaaaaaaaaaaaaaaaa"));
    sets = DisplayProfile::savedSets(settings);
    QCOMPARE(sets.size(), 1);
    QCOMPARE(sets.first().fingerprint, QStringLiteral("bbbbbbbbbbbbbbbbbbbb"));
    QVERIFY(!sets.first().saved.isEmpty());
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

void TestPlankBroker::parsesPasskeyChallenge()
{
    const auto reply = PlankBroker::parseAuthReply(200, passkeyChallengeReply());
    QCOMPARE(reply.kind, PlankBroker::ReplyKind::Challenge);
    QCOMPARE(reply.conversationId, QStringLiteral("pk-1"));
    const PlankBroker::Prompt* prompt = PlankBroker::passkeyPrompt(reply.prompts);
    QVERIFY(prompt != nullptr);
    QCOMPARE(prompt->id, QStringLiteral("passkey"));
    QCOMPARE(prompt->passkey.rpId, QStringLiteral("ipa.bde.run"));
    QCOMPARE(prompt->passkey.credentialIds, QStringList({PasskeyOtherCredential, PasskeyCredential}));
    QCOMPARE(prompt->passkey.challenge, PasskeyChallenge);
    QVERIFY(prompt->passkey.userVerification);
    // Password + code challenges carry no passkey prompt.
    const auto password = PlankBroker::parseAuthReply(200, json(R"({"state":"challenge","conversation_id":"c",
        "prompts":[{"id":"password","style":"secret","text":"Password"},{"id":"otp","style":"otp","text":"Code"}]})"));
    QCOMPARE(password.kind, PlankBroker::ReplyKind::Challenge);
    QVERIFY(PlankBroker::passkeyPrompt(password.prompts) == nullptr);
}

void TestPlankBroker::rejectsMalformedPasskeyChallenges()
{
    const QString c31 = QString::fromLatin1(QByteArray(31, 'a').toBase64());
    const QString c33 = QString::fromLatin1(QByteArray(33, 'a').toBase64());
    QString unpadded = PasskeyChallenge;
    unpadded.remove(QLatin1Char('='));
    const QString template_ = QString::fromUtf8(R"({"state":"challenge","conversation_id":"c","prompts":[{"id":"passkey",)"
                                             R"("style":"passkey","text":"Passkey","passkey":%1}]})");
    const QString good = QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})")
            .arg(PasskeyCredential, PasskeyChallenge);
    QCOMPARE(PlankBroker::parseAuthReply(200, template_.arg(good).toUtf8()).kind, PlankBroker::ReplyKind::Challenge);
    const QStringList bad = {
        QStringLiteral("null"),
        QString::fromUtf8(R"("x")"),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, c31),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, c33),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, unpadded),
        QString::fromUtf8(R"({"rp_id":"IPA.BDE.RUN","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"192.168.10.240","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"https://ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})").arg(PasskeyCredential, PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":[],"challenge":"%1"})").arg(PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":"%1","challenge":"%2"})").arg(PasskeyCredential, PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["not base64!"],"challenge":"%1"})").arg(PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":[7],"challenge":"%1"})").arg(PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2","user_verification":"yes"})")
                .arg(PasskeyCredential, PasskeyChallenge),
        QString::fromUtf8(R"({"rp_id":"ipa.bde.run","credential_ids":["%1"],"challenge":"%2"})")
                .arg(QString::fromLatin1(QByteArray(1025, 'a').toBase64()), PasskeyChallenge),
    };
    for (const QString& passkey : bad) {
        QCOMPARE(PlankBroker::parseAuthReply(200, template_.arg(passkey).toUtf8()).kind,
                 PlankBroker::ReplyKind::Malformed);
    }
    // A passkey prompt without its request object is malformed too.
    const QByteArray withoutRequest = R"({"state":"challenge","conversation_id":"c",
        "prompts":[{"id":"passkey","style":"passkey","text":"Passkey"}]})";
    QCOMPARE(PlankBroker::parseAuthReply(200, withoutRequest).kind, PlankBroker::ReplyKind::Malformed);
}

void TestPlankBroker::passwordAnswersNeverSatisfyPasskeyPrompt()
{
    const auto reply = PlankBroker::parseAuthReply(200, passkeyChallengeReply());
    QJsonArray responses;
    QVERIFY(!PlankBroker::buildResponses(reply.prompts, QStringLiteral("anna"), QStringLiteral("pw"),
                                         QStringLiteral("123456"), responses));
    QVERIFY(responses.isEmpty());
    // A mixed challenge is not a passkey challenge either.
    QVector<PlankBroker::Prompt> mixed = reply.prompts;
    mixed.append({QStringLiteral("otp"), QStringLiteral("otp"), QStringLiteral("Code"), {}});
    QVERIFY(PlankBroker::passkeyPrompt(mixed) == nullptr);
}

void TestPlankBroker::validatesPasskeyRpIdsAndUsernames()
{
    QVERIFY(PlankBroker::isPasskeyRpId(QStringLiteral("ipa.bde.run")));
    QVERIFY(PlankBroker::isPasskeyRpId(QStringLiteral("ipa-1.example.test")));
    QVERIFY(PlankBroker::isPasskeyRpId(PlankBroker::defaultPasskeyRpId()));
    for (const char* bad : {"", "IPA.bde.run", "ipa.bde.run.", ".ipa", "ipa..run", "-ipa.run", "ipa-.run",
                            "10.0.0.1", "ipa.bde.run:443", "ipa bde", "ipa/bde"}) {
        QVERIFY2(!PlankBroker::isPasskeyRpId(QString::fromLatin1(bad)), bad);
    }
    QCOMPARE(PlankBroker::normalizePasskeyUsername(QStringLiteral("  Anna ")), QStringLiteral("anna"));
    QVERIFY(PlankBroker::isPasskeyUsername(QStringLiteral("anna")));
    QVERIFY(PlankBroker::isPasskeyUsername(QStringLiteral("anna.m-b_2")));
    for (const char* bad : {"", "Anna", ".anna", "-anna", "../x", "a/b", "anna smith"}) {
        QVERIFY2(!PlankBroker::isPasskeyUsername(QString::fromLatin1(bad)), bad);
    }
}

void TestPlankBroker::buildsPasskeyHelperInput()
{
    const QJsonObject input = QJsonDocument::fromJson(PlankBroker::passkeyHelperInput(passkeyRequest())).object();
    QCOMPARE(input.value(QStringLiteral("rp_id")).toString(), QStringLiteral("ipa.bde.run"));
    QCOMPARE(input.value(QStringLiteral("challenge")).toString(), PasskeyChallenge);
    QCOMPARE(input.value(QStringLiteral("credential_ids")).toArray(),
             QJsonArray({PasskeyOtherCredential, PasskeyCredential}));
    QCOMPARE(input.value(QStringLiteral("user_verification")).toBool(), true);
    QCOMPARE(input.size(), 4);
}

void TestPlankBroker::validatesPasskeyAssertion()
{
    const PlankBroker::PasskeyRequest request = passkeyRequest();
    const QByteArray authData = passkeyAuthData(request.rpId);
    const QByteArray signature(71, '\x30');
    PlankBroker::PasskeyAssertion assertion;
    QVERIFY(PlankBroker::parsePasskeyAssertion(assertionJson(PasskeyCredential, authData, signature) + "\n",
                                               request, assertion));
    QCOMPARE(assertion.credentialId, PasskeyCredential);
    QCOMPARE(assertion.authenticatorData, QString::fromLatin1(authData.toBase64()));
    QCOMPARE(assertion.signature, QString::fromLatin1(signature.toBase64()));

    const QString unknown = QString::fromLatin1(QByteArray(32, '\x33').toBase64());
    const QByteArray rejected[] = {
        assertionJson(unknown, authData, signature),                                    // not an allowed id
        assertionJson(PasskeyCredential, passkeyAuthData(QStringLiteral("evil.example")), signature),
        assertionJson(PasskeyCredential, passkeyAuthData(request.rpId, 0x04), signature), // UP clear
        assertionJson(PasskeyCredential, authData.left(36), signature),
        assertionJson(PasskeyCredential, authData + QByteArray(1024, 'x'), signature),
        assertionJson(PasskeyCredential, authData, QByteArray()),
        assertionJson(PasskeyCredential, authData, QByteArray(257, '\x30')),
        QByteArray("{}"),
        QByteArray("not json"),
        QByteArray("{\"credential_id\":\"") + PasskeyCredential.toLatin1() +
                "\",\"authenticator_data\":\"***\",\"signature\":\"MEUC\"}",
    };
    for (const QByteArray& output : rejected) {
        QVERIFY2(!PlankBroker::parsePasskeyAssertion(output, request, assertion), output.constData());
        QVERIFY(assertion.credentialId.isEmpty());
    }
}

void TestPlankBroker::tlsPasskeyStartAndRespond()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", passkeyChallengeReply());
    server.queue("200 OK", R"({"state":"authenticated","session_token":"tok-pk","expires_in":36000,"username":"anna"})");
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const QByteArray authData = passkeyAuthData(QStringLiteral("ipa.bde.run"));
    const QByteArray signature = QByteArray::fromHex("3044022001020304050607080910111213141516171819202122232425262728293031"
                                                     "3202200102030405060708091011121314151617181920212223242526272829303132");
    int assertions = 0;
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [&](const PlankBroker::PasskeyRequest& request, PlankBroker::PasskeyAssertion& assertion) {
        ++assertions;
        [&] { QCOMPARE(request.challenge, PasskeyChallenge); }();
        return PlankBroker::parsePasskeyAssertion(assertionJson(PasskeyCredential, authData, signature), request, assertion) ?
                    PlankBrokerClient::PasskeyAssertResult::Signed : PlankBrokerClient::PasskeyAssertResult::Failed;
    });
    QCOMPARE(assertions, 1);
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Authenticated);
    QCOMPARE(outcome.reply.sessionToken, QStringLiteral("tok-pk"));
    QCOMPARE(outcome.reply.username, QStringLiteral("anna"));

    QCOMPARE(server.requestLog.size(), 2);
    QVERIFY(server.requestLog.at(0).startsWith("POST /v1/auth/start HTTP/1.1\r\n"));
    QVERIFY(server.requestLog.at(0).endsWith(R"({"method":"passkey","username":"anna"})"));
    QVERIFY(server.requestLog.at(1).startsWith("POST /v1/auth/respond HTTP/1.1\r\n"));
    const QByteArray respondBody = server.requestLog.at(1).mid(server.requestLog.at(1).indexOf("\r\n\r\n") + 4);
    const QJsonObject respond = QJsonDocument::fromJson(respondBody).object();
    QCOMPARE(respond.size(), 2);
    QCOMPARE(respond.value(QStringLiteral("conversation_id")).toString(), QStringLiteral("pk-1"));
    QVERIFY(!respond.contains(QStringLiteral("responses")));
    const QJsonObject passkey = respond.value(QStringLiteral("passkey")).toObject();
    QCOMPARE(passkey.size(), 3);
    QCOMPARE(passkey.value(QStringLiteral("credential_id")).toString(), PasskeyCredential);
    QCOMPARE(QByteArray::fromBase64(passkey.value(QStringLiteral("authenticator_data")).toString().toLatin1()), authData);
    QCOMPARE(QByteArray::fromBase64(passkey.value(QStringLiteral("signature")).toString().toLatin1()), signature);
    QVERIFY(!server.requestLog.at(1).contains("Authorization:"));
}

void TestPlankBroker::tlsPasswordStartIsUnchanged()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", R"({"state":"challenge","conversation_id":"c1","prompts":[)"
                                     R"({"id":"password","style":"secret","text":"Password"},)"
                                     R"({"id":"otp","style":"otp","text":"Authenticator code"}]})");
    server.queue("200 OK", R"({"state":"authenticated","session_token":"tok","username":"anna"})");
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const auto challenge = client.start(QStringLiteral("anna"));
    QJsonArray responses;
    QVERIFY(PlankBroker::buildResponses(challenge.prompts, QStringLiteral("anna"), QStringLiteral("pw"),
                                        QStringLiteral("123456"), responses));
    QCOMPARE(client.respond(challenge.conversationId, responses).kind, PlankBroker::ReplyKind::Authenticated);
    QCOMPARE(server.requestLog.size(), 2);
    QVERIFY(server.requestLog.at(0).endsWith("\r\n\r\n{\"username\":\"anna\"}"));
    QVERIFY(server.requestLog.at(1).endsWith("\r\n\r\n{\"conversation_id\":\"c1\",\"responses\":[\"pw\",\"123456\"]}"));
}

void TestPlankBroker::tlsPasskeyFallsBackWithoutLocalKey()
{
    // The broker answers unknown users with a synthetic challenge of the same
    // shape; the helper finds no matching key (exit 3) and nothing is sent back.
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = passkeyChallengeReply();
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        return PlankBrokerClient::PasskeyAssertResult::NoMatchingKey;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Fallback);
    QVERIFY(outcome.reply.sessionToken.isEmpty());
    QCOMPARE(server.requestLog.size(), 1);

    // A broken or missing helper falls back the same way.
    server.requestLog.clear();
    QCOMPARE(client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        return PlankBrokerClient::PasskeyAssertResult::Failed;
    }).result, PlankBrokerClient::PasskeySignIn::Fallback);
    QCOMPARE(server.requestLog.size(), 1);
}

void TestPlankBroker::tlsPasskeyFallsBackOnDenial()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", passkeyChallengeReply());
    server.queue("200 OK", R"({"state":"denied"})");
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [](const PlankBroker::PasskeyRequest& request, PlankBroker::PasskeyAssertion& assertion) {
        PlankBroker::parsePasskeyAssertion(assertionJson(PasskeyCredential, passkeyAuthData(request.rpId),
                                                         QByteArray(70, '\x30')), request, assertion);
        return PlankBrokerClient::PasskeyAssertResult::Signed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Fallback);
    QCOMPARE(server.requestLog.size(), 2);
}

void TestPlankBroker::tlsPasskeyFallsBackOnStartDenial()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = R"({"state":"denied"})";
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    bool called = false;
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [&](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        called = true;
        return PlankBrokerClient::PasskeyAssertResult::Signed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Fallback);
    QVERIFY(!called);
}

void TestPlankBroker::tlsPasskeyFallsBackWithoutPasskeyPrompt()
{
    // A broker without section 13.3 ignores "method" and asks for password + code.
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = R"({"state":"challenge","conversation_id":"c1","prompts":[)"
                  R"({"id":"password","style":"secret","text":"Password"},)"
                  R"({"id":"otp","style":"otp","text":"Authenticator code"}]})";
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    bool called = false;
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [&](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        called = true;
        return PlankBrokerClient::PasskeyAssertResult::Signed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Fallback);
    QVERIFY(!called);
    QCOMPARE(server.requestLog.size(), 1);
}

void TestPlankBroker::tlsPasskeyRefusesOtherRelyingParty()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = passkeyChallengeReply(QStringLiteral("evil.example"));
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    bool called = false;
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [&](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        called = true;
        return PlankBrokerClient::PasskeyAssertResult::Signed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Fallback);
    QVERIFY(!called);
}

void TestPlankBroker::tlsPasskeyNotConfirmed()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.body = passkeyChallengeReply();
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
        return PlankBrokerClient::PasskeyAssertResult::NotConfirmed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::NotConfirmed);
    QCOMPARE(server.requestLog.size(), 1);
}

void TestPlankBroker::tlsPasskeyRateLimitThrows()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.status = "429 Too Many Requests";
    server.body = R"({"state":"denied","retry_after":60})";
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    try {
        client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
                [](const PlankBroker::PasskeyRequest&, PlankBroker::PasskeyAssertion&) {
            return PlankBrokerClient::PasskeyAssertResult::Signed;
        });
        QFAIL("429 must throw, not fall back");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::RateLimited);
        QCOMPARE(error.retryAfter(), 60);
    }
}

void TestPlankBroker::helperExitCodesMapToOutcomes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const PlankBroker::PasskeyRequest request = passkeyRequest();
    PlankBroker::PasskeyAssertion assertion;
    using R = PlankBrokerClient::PasskeyAssertResult;

    const QString good = fakeHelper(directory, 0, assertionJson(PasskeyCredential, passkeyAuthData(request.rpId),
                                                                QByteArray(70, '\x30')));
    QCOMPARE(PlankPasskeyHelper(good).assertion(request, QStringLiteral("anna"), assertion), R::Signed);
    QCOMPARE(assertion.credentialId, PasskeyCredential);
    QCOMPARE(readFile(good + QStringLiteral(".args")), QByteArray("assert\n--rp\nipa.bde.run\n--user\nanna\n"));
    QCOMPARE(QJsonDocument::fromJson(readFile(good + QStringLiteral(".stdin"))).object(),
             QJsonDocument::fromJson(PlankBroker::passkeyHelperInput(request)).object());

    QCOMPARE(PlankPasskeyHelper(fakeHelper(directory, 3, QByteArray())).assertion(request, QStringLiteral("anna"), assertion),
             R::NoMatchingKey);
    QCOMPARE(PlankPasskeyHelper(fakeHelper(directory, 4, QByteArray())).assertion(request, QStringLiteral("anna"), assertion),
             R::NotConfirmed);
    QCOMPARE(PlankPasskeyHelper(fakeHelper(directory, 1, QByteArray())).assertion(request, QStringLiteral("anna"), assertion),
             R::Failed);
    QCOMPARE(PlankPasskeyHelper(fakeHelper(directory, 2, QByteArray())).assertion(request, QStringLiteral("anna"), assertion),
             R::Failed);
    // Exit 0 with an assertion for a credential the broker did not allow.
    const QString wrong = QString::fromLatin1(QByteArray(32, '\x44').toBase64());
    QTemporaryDir other;
    QCOMPARE(PlankPasskeyHelper(fakeHelper(other, 0, assertionJson(wrong, passkeyAuthData(request.rpId),
                                                                   QByteArray(70, '\x30'))))
             .assertion(request, QStringLiteral("anna"), assertion), R::Failed);
    QVERIFY(assertion.credentialId.isEmpty());
    // No helper at all (Linux, or a damaged bundle).
    QVERIFY(!PlankPasskeyHelper(directory.filePath(QStringLiteral("missing"))).available());
    QCOMPARE(PlankPasskeyHelper(directory.filePath(QStringLiteral("missing"))).assertion(request, QStringLiteral("anna"), assertion),
             R::Failed);
    QVERIFY(!PlankPasskeyHelper(QString()).available());
    // Invalid user names never reach the helper.
    QCOMPARE(PlankPasskeyHelper(good).assertion(request, QStringLiteral("../anna"), assertion), R::NoMatchingKey);
}

void TestPlankBroker::helperListAndCreateParsing()
{
    const QString spki = QString::fromLatin1(QByteArray(91, '\x04').toBase64());
    const QString mapping = QStringLiteral("passkey:%1,%2").arg(PasskeyCredential, spki);
    PlankPasskeyHelper::CreatedKey created;
    QVERIFY(PlankPasskeyHelper::parseCreated(QJsonDocument(QJsonObject {
        {QStringLiteral("credential_id"), PasskeyCredential}, {QStringLiteral("public_key"), spki},
        {QStringLiteral("mapping"), mapping}}).toJson(), created));
    QCOMPARE(created.mapping, mapping);
    QVERIFY(!PlankPasskeyHelper::parseCreated(QJsonDocument(QJsonObject {
        {QStringLiteral("credential_id"), PasskeyCredential}, {QStringLiteral("public_key"), spki},
        {QStringLiteral("mapping"), QStringLiteral("passkey:%1,%2").arg(PasskeyOtherCredential, spki)}}).toJson(), created));

    QVector<PlankPasskeyHelper::LocalKey> keys;
    const QJsonObject entry {
        {QStringLiteral("rp_id"), QStringLiteral("ipa.bde.run")}, {QStringLiteral("username"), QStringLiteral("anna")},
        {QStringLiteral("credential_id"), PasskeyCredential}, {QStringLiteral("public_key"), spki},
        {QStringLiteral("mapping"), mapping}, {QStringLiteral("created"), QStringLiteral("2026-09-21T20:00:00Z")}};
    QVERIFY(PlankPasskeyHelper::parseList(QJsonDocument(QJsonArray {entry}).toJson(), keys));
    QCOMPARE(keys.size(), 1);
    QCOMPARE(keys.at(0).username, QStringLiteral("anna"));
    QVERIFY(PlankPasskeyHelper::parseList("[]\n", keys));
    QVERIFY(keys.isEmpty());
    QJsonObject badUser = entry;
    badUser.insert(QStringLiteral("username"), QStringLiteral("../x"));
    QVERIFY(!PlankPasskeyHelper::parseList(QJsonDocument(QJsonArray {badUser}).toJson(), keys));
    QVERIFY(!PlankPasskeyHelper::parseList("{}", keys));

    QTemporaryDir directory;
    const QString fake = fakeHelper(directory, 0, QJsonDocument(QJsonArray {entry}).toJson());
    QVERIFY(PlankPasskeyHelper(fake).list(QStringLiteral("ipa.bde.run"), keys));
    QCOMPARE(keys.size(), 1);
    QCOMPARE(readFile(fake + QStringLiteral(".args")), QByteArray("list\n--rp\nipa.bde.run\n"));
}

void TestPlankBroker::realHelperWithoutKeyFallsBack()
{
    // The real helper, when the build provides it: an empty key store gives
    // exit 3 (fallback), and its software-key self-test output passes the
    // same validation as a Touch ID assertion.
    const QString program = qEnvironmentVariable("PLANK_PASSKEY_HELPER");
    if (program.isEmpty()) QSKIP("PLANK_PASSKEY_HELPER not set");
    QVERIFY(QFileInfo(program).isExecutable());
    QTemporaryDir store;
    QVERIFY(store.isValid());
    qputenv("PLANK_PASSKEY_STORE", store.path().toUtf8());
    const PlankPasskeyHelper helper(program);
    const PlankBroker::PasskeyRequest request = passkeyRequest();
    PlankBroker::PasskeyAssertion assertion;
    QCOMPARE(helper.assertion(request, QStringLiteral("anna"), assertion),
             PlankBrokerClient::PasskeyAssertResult::NoMatchingKey);
    QVector<PlankPasskeyHelper::LocalKey> keys;
    QVERIFY(helper.list(QStringLiteral("ipa.bde.run"), keys));
    QVERIFY(keys.isEmpty());
    const PlankPasskeyHelper::Result selfTest = helper.run({QStringLiteral("self-test")},
                                                           PlankBroker::passkeyHelperInput(request),
                                                           PlankPasskeyHelper::QuickTimeoutMs);
    QVERIFY(selfTest.ok());
    QVERIFY(PlankBroker::parsePasskeyAssertion(selfTest.output, request, assertion));
    QCOMPARE(QByteArray::fromBase64(assertion.authenticatorData.toLatin1()), passkeyAuthData(request.rpId));
    qunsetenv("PLANK_PASSKEY_STORE");
}

// ---------------------------------------------------------------------------
// Device-bound sessions (bde-linux docs/plank-broker.md section 14)
// ---------------------------------------------------------------------------

void TestPlankBroker::deviceProofMessageFormat()
{
    // Golden string; hashes computed independently with `shasum -a 256`.
    QCOMPARE(PlankBroker::deviceProofMessage("POST", "/v1/hosts/ws01.example.test/connect", 1790000000,
                                             QStringLiteral("tok"), "{}"),
             QByteArray("plank-device-proof-v1\n"
                        "POST\n"
                        "/v1/hosts/ws01.example.test/connect\n"
                        "1790000000\n"
                        "1a7674eb4ee78df7e1ac439a93c3fa8e3c945784d4dec9fd8e3011738b2f1d62\n"
                        "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"));
    // GET: empty body; method upper-cased; query never part of the path.
    QCOMPARE(PlankBroker::deviceProofMessage("get", "/v1/hosts?x=1", 1, QStringLiteral("tok"), QByteArray()),
             QByteArray("plank-device-proof-v1\nGET\n/v1/hosts\n1\n"
                        "1a7674eb4ee78df7e1ac439a93c3fa8e3c945784d4dec9fd8e3011738b2f1d62\n"
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

void TestPlankBroker::validatesDeviceKeysAndSignatures()
{
    QVERIFY(PlankBroker::isDevicePublicKey(DeviceSpki));
    QVERIFY(!PlankBroker::isDevicePublicKey(QString()));
    QVERIFY(!PlankBroker::isDevicePublicKey(QString::fromLatin1(QByteArray(201, '\x30').toBase64())));
    QVERIFY(PlankBroker::isDevicePublicKey(QString::fromLatin1(QByteArray(200, '\x30').toBase64())));
    QVERIFY(!PlankBroker::isDevicePublicKey(QString::fromLatin1(QByteArray(91, '\x04').toBase64())));
    QVERIFY(!PlankBroker::isDevicePublicKey(DeviceSpki.left(DeviceSpki.size() - 1)));
    QVERIFY(!PlankBroker::isDevicePublicKey(DeviceSpki + QStringLiteral("\n")));
    QVERIFY(PlankBroker::isDeviceSignature(fakeDeviceSignature(1)));
    QVERIFY(!PlankBroker::isDeviceSignature(QString()));
    QVERIFY(!PlankBroker::isDeviceSignature(QString::fromLatin1(QByteArray(73, '\x30').toBase64())));
    QVERIFY(!PlankBroker::isDeviceSignature(QString::fromLatin1(QByteArray(70, '\x02').toBase64())));
    QVERIFY(!PlankBroker::isDeviceSignature(QStringLiteral("not base64!")));
}

void TestPlankBroker::parsesDeviceBound()
{
    auto parse = [](const char* body) { return PlankBroker::parseAuthReply(200, QByteArray(body)); };
    const auto bound = parse(R"({"state":"authenticated","session_token":"t","expires_in":2592000,"device_bound":true})");
    QCOMPARE(bound.kind, PlankBroker::ReplyKind::Authenticated);
    QVERIFY(bound.deviceBound);
    QCOMPARE(bound.expiresIn, 2592000);
    const auto absent = parse(R"({"state":"authenticated","session_token":"t","expires_in":36000})");
    QCOMPARE(absent.kind, PlankBroker::ReplyKind::Authenticated);
    QVERIFY(!absent.deviceBound);
    const auto unbound = parse(R"({"state":"authenticated","session_token":"t","device_bound":false})");
    QCOMPARE(unbound.kind, PlankBroker::ReplyKind::Authenticated);
    QVERIFY(!unbound.deviceBound);
    // Tolerated, never taken as bound.
    const auto odd = parse(R"({"state":"authenticated","session_token":"t","device_bound":"yes"})");
    QCOMPARE(odd.kind, PlankBroker::ReplyKind::Authenticated);
    QVERIFY(!odd.deviceBound);
}

void TestPlankBroker::tlsStartSendsDeviceKeyForBothMethods()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    const QByteArray passwordChallenge = R"({"state":"challenge","conversation_id":"c1","prompts":[)"
                                         R"({"id":"password","style":"secret","text":"Password"},)"
                                         R"({"id":"otp","style":"otp","text":"Authenticator code"}]})";
    server.queue("200 OK", passwordChallenge);
    server.queue("200 OK", passkeyChallengeReply());
    server.queue("200 OK", passwordChallenge);
    server.queue("200 OK", passkeyChallengeReply());
    PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
    int provided = 0;
    config.devicePublicKey = [&]() { ++provided; return DeviceSpki; };
    const PlankBrokerClient bound(config);
    QCOMPARE(bound.start(QStringLiteral("anna")).kind, PlankBroker::ReplyKind::Challenge);
    QCOMPARE(bound.start(QStringLiteral("anna"), PlankBroker::AuthMethod::Passkey).kind, PlankBroker::ReplyKind::Challenge);
    QCOMPARE(provided, 2);
    const PlankBrokerClient unbound(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    QCOMPARE(unbound.start(QStringLiteral("anna")).kind, PlankBroker::ReplyKind::Challenge);
    QCOMPARE(unbound.start(QStringLiteral("anna"), PlankBroker::AuthMethod::Passkey).kind, PlankBroker::ReplyKind::Challenge);

    QCOMPARE(server.requestLog.size(), 4);
    QCOMPARE(parseRecordedRequest(server.requestLog.at(0)).body,
             QStringLiteral(R"({"device_key":"%1","username":"anna"})").arg(DeviceSpki).toUtf8());
    QCOMPARE(parseRecordedRequest(server.requestLog.at(1)).body,
             QStringLiteral(R"({"device_key":"%1","method":"passkey","username":"anna"})").arg(DeviceSpki).toUtf8());
    QCOMPARE(parseRecordedRequest(server.requestLog.at(2)).body, QByteArray(R"({"username":"anna"})"));
    QCOMPARE(parseRecordedRequest(server.requestLog.at(3)).body, QByteArray(R"({"method":"passkey","username":"anna"})"));
    for (const QByteArray& raw : server.requestLog) {
        const RecordedRequest request = parseRecordedRequest(raw);
        QCOMPARE(request.target, QByteArray("/v1/auth/start"));
        // No bearer, so no proof, on sign-in requests.
        QVERIFY(!request.headers.contains("authorization"));
        QVERIFY(!request.headers.contains("x-plank-device-proof"));
        QVERIFY(!request.headers.contains("x-plank-device-time"));
    }
}

void TestPlankBroker::tlsStartOmitsUnusableDeviceKey()
{
    // Helper missing or failing (empty) or printing garbage: sign in unbound.
    for (const QString& key : {QString(), QStringLiteral("not base64!"), QString::fromLatin1(QByteArray(201, '\x30').toBase64())}) {
        TestBrokerServer server(QSsl::TlsV1_3OrLater);
        QVERIFY(server.listen());
        server.body = passkeyChallengeReply();
        PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
        config.devicePublicKey = [key]() { return key; };
        QCOMPARE(PlankBrokerClient(config).start(QStringLiteral("anna"), PlankBroker::AuthMethod::Passkey).kind,
                 PlankBroker::ReplyKind::Challenge);
        QCOMPARE(server.requestLog.size(), 1);
        QCOMPARE(parseRecordedRequest(server.requestLog.at(0)).body, QByteArray(R"({"method":"passkey","username":"anna"})"));
    }
}

void TestPlankBroker::tlsPasskeySignInSendsDeviceKey()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", passkeyChallengeReply());
    server.queue("200 OK", R"({"state":"authenticated","session_token":"tok-pk","expires_in":2592000,)"
                           R"("username":"anna","device_bound":true})");
    PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
    config.devicePublicKey = []() { return DeviceSpki; };
    const PlankBrokerClient client(config);
    const QByteArray authData = passkeyAuthData(QStringLiteral("ipa.bde.run"));
    const auto outcome = client.signInWithPasskey(QStringLiteral("anna"), QStringLiteral("ipa.bde.run"),
            [&](const PlankBroker::PasskeyRequest& request, PlankBroker::PasskeyAssertion& assertion) {
        return PlankBroker::parsePasskeyAssertion(assertionJson(PasskeyCredential, authData,
                                                                QByteArray::fromBase64(fakeDeviceSignature(1).toLatin1())),
                                                  request, assertion) ?
                    PlankBrokerClient::PasskeyAssertResult::Signed : PlankBrokerClient::PasskeyAssertResult::Failed;
    });
    QCOMPARE(outcome.result, PlankBrokerClient::PasskeySignIn::Authenticated);
    QVERIFY(outcome.reply.deviceBound);
    QCOMPARE(outcome.reply.expiresIn, 2592000);
    QCOMPARE(server.requestLog.size(), 2);
    QCOMPARE(parseRecordedRequest(server.requestLog.at(0)).body,
             QStringLiteral(R"({"device_key":"%1","method":"passkey","username":"anna"})").arg(DeviceSpki).toUtf8());
    // The key rides along the conversation; /v1/auth/respond does not repeat it.
    QVERIFY(!server.requestLog.at(1).contains("device_key"));
}

void TestPlankBroker::tlsBearerCallsCarryDeviceProof()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", R"({"hosts":[{"id":"ws01.example.test","name":"ws01","online":true,)"
                           R"("in_use_by":null,"connectable":true,"reason":null}]})");
    server.queue("200 OK", QStringLiteral(R"({"route":"relay","endpoint":"127.0.0.1","port":29001,)"
                                          R"("host_cert_sha256":"%1","username":"anna","gssapi_token":"YWJj",)"
                                          R"("expires_in":30})").arg(HostPin).toUtf8());
    server.queue("200 OK", R"({"state":"ok"})");
    server.queue("200 OK", R"({"state":"ok"})");
    PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
    QList<QByteArray> signedMessages;
    QList<QString> signatures;
    config.deviceSigner = [&](const QByteArray& message) {
        signedMessages.append(message);
        signatures.append(fakeDeviceSignature(signedMessages.size()));
        return PlankBrokerClient::DeviceSignature {PlankBrokerClient::DeviceSignature::Signed, signatures.last()};
    };
    const QString token = QStringLiteral("bound-session-token");
    const qint64 before = QDateTime::currentSecsSinceEpoch();
    const PlankBrokerClient client(config);
    QCOMPARE(client.hosts(token).size(), 1);
    QCOMPARE(client.connect(token, QStringLiteral("ws01.example.test")).port, quint16(29001));
    client.keepalive(token, QStringLiteral("ws01.example.test"));
    client.logout(token);
    const qint64 after = QDateTime::currentSecsSinceEpoch();

    QCOMPARE(server.requestLog.size(), 4);
    QCOMPARE(signedMessages.size(), 4);
    const QList<QPair<QByteArray, QByteArray>> expected {
        {"GET", "/v1/hosts"},
        {"POST", "/v1/hosts/ws01.example.test/connect"},
        {"POST", "/v1/hosts/ws01.example.test/keepalive"},
        {"POST", "/v1/logout"},
    };
    const QByteArray tokenHash = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex();
    for (int i = 0; i < expected.size(); ++i) {
        const RecordedRequest request = parseRecordedRequest(server.requestLog.at(i));
        QCOMPARE(request.method, expected.at(i).first);
        QCOMPARE(request.target, expected.at(i).second);
        QCOMPARE(request.body, QByteArray(request.method == "GET" ? "" : "{}"));
        QCOMPARE(request.headers.value("authorization"), "Bearer " + token.toLatin1());
        const QByteArray time = request.headers.value("x-plank-device-time");
        bool numeric = false;
        const qint64 seconds = time.toLongLong(&numeric);
        QVERIFY(numeric);
        QVERIFY(seconds >= before && seconds <= after);
        QCOMPARE(request.headers.value("x-plank-device-proof"), signatures.at(i).toLatin1());
        // The signed message is the canonical string rebuilt from the bytes
        // the broker actually received.
        const QByteArray canonical = "plank-device-proof-v1\n" + request.method + "\n" + request.target + "\n" + time +
                "\n" + tokenHash + "\n" + QCryptographicHash::hash(request.body, QCryptographicHash::Sha256).toHex();
        QCOMPARE(signedMessages.at(i), canonical);
        QCOMPARE(signedMessages.at(i), PlankBroker::deviceProofMessage(request.method, request.target, seconds,
                                                                        token, request.body));
    }
}

void TestPlankBroker::tlsBearerCallsWithoutSignerAreUnchanged()
{
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", R"({"hosts":[]})");
    server.queue("200 OK", R"({"state":"ok"})");
    server.queue("200 OK", R"({"state":"ok"})");
    const PlankBrokerClient client(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    QVERIFY(client.hosts(QStringLiteral("t")).isEmpty());
    client.keepalive(QStringLiteral("t"), QStringLiteral("ws01.example.test"));
    client.logout(QStringLiteral("t"));
    QCOMPARE(server.requestLog.size(), 3);
    for (const QByteArray& raw : server.requestLog) {
        QVERIFY(!raw.toLower().contains("x-plank-device"));
        QVERIFY(raw.contains("Authorization: Bearer t\r\n"));
    }
}

void TestPlankBroker::tlsDeviceSigningFailureSendsNoProof()
{
    using Signature = PlankBrokerClient::DeviceSignature;
    // No device key for this broker: the session is unbound, the request goes
    // out as before, without proof headers.
    {
        TestBrokerServer server(QSsl::TlsV1_3OrLater);
        QVERIFY(server.listen());
        server.body = R"({"hosts":[]})";
        PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
        int calls = 0;
        config.deviceSigner = [&](const QByteArray&) { ++calls; return Signature {Signature::NoKey, QString()}; };
        QVERIFY(PlankBrokerClient(config).hosts(QStringLiteral("t")).isEmpty());
        QCOMPARE(calls, 1);
        QCOMPARE(server.requestLog.size(), 1);
        QVERIFY(!server.requestLog.at(0).toLower().contains("x-plank-device"));
        QVERIFY(server.requestLog.at(0).contains("Authorization: Bearer t\r\n"));
    }
    // Signing impossible right now (locked Mac, helper error) or a malformed
    // signature: nothing is sent and the error is a retryable Network one, so
    // a bound session is never signed out for it.
    for (const Signature& signature : {Signature {Signature::Failed, QString()},
                                       Signature {Signature::Signed, QStringLiteral("garbage")},
                                       Signature {Signature::Signed, QString::fromLatin1(QByteArray(90, '\x30').toBase64())}}) {
        TestBrokerServer server(QSsl::TlsV1_3OrLater);
        QVERIFY(server.listen());
        server.body = R"({"hosts":[]})";
        PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
        config.deviceSigner = [&](const QByteArray&) { return signature; };
        try {
            PlankBrokerClient(config).hosts(QStringLiteral("t"));
            QFAIL("a failed device signature must not send the request");
        } catch (const PlankBrokerError& error) {
            QCOMPARE(error.kind(), PlankBrokerError::Network);
        }
        QCoreApplication::processEvents();
        QVERIFY(server.requestLog.isEmpty());
    }
}

void TestPlankBroker::tlsDeviceBoundSessionRejectionSignsOut()
{
    // A bound session without a valid proof is answered 401: SessionExpired.
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.status = "401 Unauthorized";
    server.body = R"({"state":"denied"})";
    PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
    config.deviceSigner = [](const QByteArray&) {
        return PlankBrokerClient::DeviceSignature {PlankBrokerClient::DeviceSignature::NoKey, QString()};
    };
    try {
        PlankBrokerClient(config).hosts(QStringLiteral("t"));
        QFAIL("401 must throw");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::SessionExpired);
    }
}

void TestPlankBroker::helperDeviceKeyCommands()
{
    // parse
    QCOMPARE(PlankPasskeyHelper::parseDevicePublicKey(QStringLiteral(R"({"public_key":"%1"})").arg(DeviceSpki).toUtf8() + "\n"),
             DeviceSpki);
    QVERIFY(PlankPasskeyHelper::parseDevicePublicKey(R"({"public_key":"x"})").isEmpty());
    QVERIFY(PlankPasskeyHelper::parseDevicePublicKey(QStringLiteral(R"({"public_key":"%1","extra":1})").arg(DeviceSpki).toUtf8()).isEmpty());
    QVERIFY(PlankPasskeyHelper::parseDevicePublicKey("[]").isEmpty());
    const QString signature = fakeDeviceSignature(7);
    QCOMPARE(PlankPasskeyHelper::parseDeviceSignature(QStringLiteral(R"({"signature":"%1"})").arg(signature).toUtf8()), signature);
    QVERIFY(PlankPasskeyHelper::parseDeviceSignature(R"({"signature":""})").isEmpty());

    // public: argv, no stdin, parsed output.
    QTemporaryDir directory;
    const QString publicHelper = fakeHelper(directory, 0, QStringLiteral(R"({"public_key":"%1"})").arg(DeviceSpki).toUtf8());
    QCOMPARE(PlankPasskeyHelper(publicHelper).devicePublicKey(QStringLiteral("remote.bde.run")), DeviceSpki);
    QCOMPARE(readFile(publicHelper + QStringLiteral(".args")), QByteArray("device-key\npublic\n--broker\nremote.bde.run\n"));
    QVERIFY(readFile(publicHelper + QStringLiteral(".stdin")).isEmpty());

    // sign: argv and {"message":"<b64>"} on stdin.
    QTemporaryDir signDirectory;
    const QString signHelper = fakeHelper(signDirectory, 0, QStringLiteral(R"({"signature":"%1"})").arg(signature).toUtf8());
    const QByteArray message = PlankBroker::deviceProofMessage("GET", "/v1/hosts", 1790000000, QStringLiteral("t"), QByteArray());
    const PlankBrokerClient::DeviceSignature signedResult = PlankPasskeyHelper(signHelper).deviceSign(QStringLiteral("remote.bde.run"), message);
    QCOMPARE(signedResult.status, PlankBrokerClient::DeviceSignature::Signed);
    QCOMPARE(signedResult.signature, signature);
    QCOMPARE(readFile(signHelper + QStringLiteral(".args")), QByteArray("device-key\nsign\n--broker\nremote.bde.run\n"));
    const QJsonObject input = QJsonDocument::fromJson(readFile(signHelper + QStringLiteral(".stdin"))).object();
    QCOMPARE(input.size(), 1);
    QCOMPARE(QByteArray::fromBase64(input.value(QStringLiteral("message")).toString().toLatin1()), message);

    // No key (exit 3): NoKey (unbound). Any other failure: Failed (not sent).
    QTemporaryDir noKey;
    QCOMPARE(PlankPasskeyHelper(fakeHelper(noKey, 3, QByteArray())).deviceSign(QStringLiteral("remote.bde.run"), message).status,
             PlankBrokerClient::DeviceSignature::NoKey);
    QTemporaryDir signFailing;
    QCOMPARE(PlankPasskeyHelper(fakeHelper(signFailing, 1, QByteArray())).deviceSign(QStringLiteral("remote.bde.run"), message).status,
             PlankBrokerClient::DeviceSignature::Failed);
    QTemporaryDir failing;
    const QString failingHelper = fakeHelper(failing, 1, QStringLiteral(R"({"public_key":"%1"})").arg(DeviceSpki).toUtf8());
    QVERIFY(PlankPasskeyHelper(failingHelper).devicePublicKey(QStringLiteral("remote.bde.run")).isEmpty());
    QVERIFY(PlankPasskeyHelper(directory.filePath(QStringLiteral("missing"))).devicePublicKey(QStringLiteral("remote.bde.run")).isEmpty());
    QCOMPARE(PlankPasskeyHelper(QString()).deviceSign(QStringLiteral("remote.bde.run"), message).status,
             PlankBrokerClient::DeviceSignature::NoKey);

    // IP literals, upper case, ports and paths never reach the helper.
    QTemporaryDir untouched;
    const QString untouchedHelper = fakeHelper(untouched, 0, QStringLiteral(R"({"public_key":"%1"})").arg(DeviceSpki).toUtf8());
    for (const QString& host : {QStringLiteral("192.168.10.229"), QStringLiteral("Remote.BDE.run"),
                                QStringLiteral("remote.bde.run:29000"), QStringLiteral("../x"), QString()}) {
        QVERIFY(PlankPasskeyHelper(untouchedHelper).devicePublicKey(host).isEmpty());
        QCOMPARE(PlankPasskeyHelper(untouchedHelper).deviceSign(host, message).status,
                 PlankBrokerClient::DeviceSignature::NoKey);
    }
    QVERIFY(!QFileInfo::exists(untouchedHelper + QStringLiteral(".args")));
}

void TestPlankBroker::realHelperDeviceKeyWithoutKey()
{
    // The real helper, when the build provides it: without a device key `sign`
    // exits 3 and creates nothing; the software-key self-test's output passes
    // the Client's signature validation. (OpenSSL verification of the
    // signature: tests/plankpasskey/test-plank-passkey.sh.)
    const QString program = qEnvironmentVariable("PLANK_PASSKEY_HELPER");
    if (program.isEmpty()) QSKIP("PLANK_PASSKEY_HELPER not set");
    QTemporaryDir store;
    QVERIFY(store.isValid());
    const QString storePath = store.filePath(QStringLiteral("device-keys"));
    qputenv("PLANK_DEVICE_KEY_STORE", storePath.toUtf8());
    const PlankPasskeyHelper helper(program);
    const QByteArray message = PlankBroker::deviceProofMessage("GET", "/v1/hosts", 1790000000, QStringLiteral("t"), QByteArray());
    QCOMPARE(helper.deviceSign(QStringLiteral("remote.bde.run"), message).status, PlankBrokerClient::DeviceSignature::NoKey);
    const QByteArray input = QJsonDocument(QJsonObject {{QStringLiteral("message"), QString::fromLatin1(message.toBase64())}})
            .toJson(QJsonDocument::Compact);
    const PlankPasskeyHelper::Result noKey = helper.run({QStringLiteral("device-key"), QStringLiteral("sign"),
                                                         QStringLiteral("--broker"), QStringLiteral("remote.bde.run")},
                                                        input, PlankPasskeyHelper::DeviceKeyTimeoutMs);
    QVERIFY(noKey.ran);
    QCOMPARE(noKey.exitCode, int(PlankPasskeyHelper::NoMatchingKey));
    QVERIFY(!QFileInfo::exists(storePath));
    const PlankPasskeyHelper::Result selfTest = helper.run({QStringLiteral("device-key"), QStringLiteral("self-test")},
                                                           input, PlankPasskeyHelper::DeviceKeyTimeoutMs);
    QVERIFY(selfTest.ok());
    const QJsonObject output = QJsonDocument::fromJson(selfTest.output).object();
    QVERIFY(PlankBroker::isDevicePublicKey(output.value(QStringLiteral("public_key")).toString()));
    QVERIFY(PlankBroker::isDeviceSignature(output.value(QStringLiteral("signature")).toString()));
    qunsetenv("PLANK_DEVICE_KEY_STORE");
}

// ---------------------------------------------------------------------------
// First sign-in wizard (bde-linux docs/plank-broker.md section 16)
// ---------------------------------------------------------------------------

namespace {

// Reference symbols from an independent encoder (libqrencode 4.1.1:
// `qrencode -l M -8 -t ASCII -m 0 <data>`), '#' = dark.
const char* const HelloVersion1Mask3[] = {
        "#######.#..#..#######",
        "#.....#.####..#.....#",
        "#.###.#...#.#.#.###.#",
        "#.###.#.#.#.#.#.###.#",
        "#.###.#....#..#.###.#",
        "#.....#....##.#.....#",
        "#######.#.#.#.#######",
        "........#..##........",
        "#.##.###.#.##.#..#.##",
        ".##.##.#.######..##..",
        "#...#.#..#.#.......##",
        "#.##...#...#..####.#.",
        ".#.######...#..#..#.#",
        "........####..#...#.#",
        "#######.#..##..#.....",
        "#.....#.#.#....#####.",
        "#.###.#.....######.##",
        "#.###.#.#.##..#.####.",
        "#.###.#.##..#.##..#..",
        "#.....#...#..#.##...#",
        "#######.#.#..#.#....."
};
const char OtpauthVectorUri[] =
        "otpauth://totp/BDE%20Fernweh:anna?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=BDE%20Fernweh";
const char* const OtpauthVersion6Mask6[] = {
        "#######.###..#.#.#.....#...#.#..#.#######",
        "#.....#.#..#####..##.##.###....#..#.....#",
        "#.###.#.#..####.#.....#.##.##..#..#.###.#",
        "#.###.#..#...####.#.#..#..##.#.##.#.###.#",
        "#.###.#.###..#.#.#...#.#..#....#..#.###.#",
        "#.....#...#####.#.#.#..#....#...#.#.....#",
        "#######.#.#.#.#.#.#.#.#.#.#.#.#.#.#######",
        ".........#..#...#.##..##.##.#...#........",
        "#..#######..##.#.#.##.#.#..#.###.#..#.###",
        "#.##.#..#.##..##.###.#.#####..###.####..#",
        ".######.#.##...#.##..#.#...#.#..#..#.#...",
        "#...##..#.....#.###.#..#..#..#.#.###.#.##",
        ".###..#.#..#.###.#.##.##.#.##...####.....",
        "#..#....#..###.....#..#.#####.#..#####.#.",
        "####.###.##.##.......##..#.#.###..#..#..#",
        "...#...##.###..#...#..#.#.#....#.###..##.",
        "..#..###.##.####...#...#...#.#.......#.#.",
        "##.#.#...#.#.##...###.##.#...##.##..#....",
        "#.#########.##.##..#.####..#..##....###.#",
        "##...#.####..#.####.#.#.#.#.#.#...#.#.##.",
        ".#..#######.###....#.##..###.#.##.#.....#",
        "#.##.#.#.####...###......###.###....#.#..",
        "#.##.##.##..#.......#..#.#.##....#..#....",
        "..####......##.#.##...#.#.#..#.##...##...",
        "###.######..####...##.####.#......#####..",
        "##..##...#.#.#.##.....#...###...##.##.###",
        "##....##..##..###.##..#.#..#.###.##.##.##",
        ".#####.#.##.....#.##......##....#...###..",
        "#.#..#####.#.#.#.#...####......###.#.#.#.",
        "##..##.#.#....#.###...##.#.#.#..##..#.##.",
        "###.####....###..#.#....#.##.#.##.#.##.##",
        "###.........#.##...##..#.#.##.###...#.###",
        "##.#.##..#.#.#####.##..#...##..#########.",
        "........#...###.#..#.##......#.##...#....",
        "#######.#####.#..##...##..##....#.#.#.#..",
        "#.....#.######.###...##.#.#.#####...##.#.",
        "#.###.#.#.#..##.###..#.#.####..######.#.#",
        "#.###.#.#...##...#.#.....#.##....#....##.",
        "#.###.#....##.....##.##.#..##..#.#.###..#",
        "#.....#..##....#.#....###.###.##..#.###.#",
        "#######.#...........#####..#....#..##.#.."
};

template <size_t Rows>
bool matrixEquals(const QrEncoder::Matrix& matrix, const char* const (&rows)[Rows])
{
    if (matrix.size != int(Rows)) return false;
    for (int y = 0; y < matrix.size; ++y) {
        if (int(qstrlen(rows[y])) != matrix.size) return false;
        for (int x = 0; x < matrix.size; ++x) {
            if (matrix.dark(x, y) != (rows[y][x] == '#')) return false;
        }
    }
    return true;
}

const char EnrollSecret[] = "JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP";

QByteArray enrollTotpReply(const char* secret = EnrollSecret)
{
    return QStringLiteral(R"({"state":"totp","otpauth_uri":"otpauth://totp/BDE%20Fernweh:anna?secret=%1&issuer=BDE%20Fernweh","secret":"%1"})")
            .arg(QString::fromLatin1(secret)).toUtf8();
}

const QByteArray EnrollStartReply = R"({"conversation_id":"en-1","prompts":[{"id":"password","style":"secret"}]})";
const QByteArray EnrollNewPasswordReply = R"({"state":"new_password","policy":{"min_length":12,"min_classes":3}})";
const QByteArray EnrollDone = R"({"state":"done"})";

QJsonObject requestJson(const QByteArray& raw)
{
    return QJsonDocument::fromJson(parseRecordedRequest(raw).body).object();
}

QByteArray requestTarget(const QByteArray& raw)
{
    return parseRecordedRequest(raw).target;
}

QString passkeyMapping()
{
    return QStringLiteral("passkey:%1,%2").arg(PasskeyCredential, DeviceSpki);
}

}

void TestPlankBroker::qrEncoderMatchesReferenceVectors()
{
    const QrEncoder::Matrix hello = QrEncoder::encode("HELLO", QrEncoder::Ecc::Medium, 3);
    QVERIFY(hello.isValid());
    QCOMPARE(hello.version, 1);
    QVERIFY(matrixEquals(hello, HelloVersion1Mask3));

    const QrEncoder::Matrix uri = QrEncoder::encode(OtpauthVectorUri, QrEncoder::Ecc::Medium, 6);
    QCOMPARE(uri.version, 6);
    QCOMPARE(uri.size, 41);
    QVERIFY(matrixEquals(uri, OtpauthVersion6Mask6));

    // Automatic mask selection: same symbol version, a valid mask, and for
    // this input the same choice as the reference encoder.
    const QrEncoder::Matrix automatic = QrEncoder::encode(OtpauthVectorUri);
    QCOMPARE(automatic.version, 6);
    QCOMPARE(automatic.mask, 6);
    QVERIFY(matrixEquals(automatic, OtpauthVersion6Mask6));
}

void TestPlankBroker::qrEncoderFormatInformationRoundTrip()
{
    // Decode the 15-bit format information (first copy) back out of the
    // symbol: level and mask must be what was asked for, the BCH remainder
    // must check, and the second copy must agree.
    const QList<QPair<QrEncoder::Ecc, int>> levels = {
        {QrEncoder::Ecc::Low, 1}, {QrEncoder::Ecc::Medium, 0}, {QrEncoder::Ecc::Quartile, 3}, {QrEncoder::Ecc::High, 2},
    };
    for (const auto& level : levels) {
        for (int mask = 0; mask < 8; ++mask) {
            const QrEncoder::Matrix matrix = QrEncoder::encode("otpauth://totp/x?secret=ABC", level.first, mask);
            QVERIFY(matrix.isValid());
            int first = 0;
            int second = 0;
            for (int i = 0; i <= 5; ++i) first |= matrix.dark(8, i) << i;
            first |= matrix.dark(8, 7) << 6 | matrix.dark(8, 8) << 7 | matrix.dark(7, 8) << 8;
            for (int i = 9; i < 15; ++i) first |= matrix.dark(14 - i, 8) << i;
            for (int i = 0; i < 8; ++i) second |= matrix.dark(matrix.size - 1 - i, 8) << i;
            for (int i = 8; i < 15; ++i) second |= matrix.dark(8, matrix.size - 15 + i) << i;
            QCOMPARE(first, second);
            const int bits = first ^ 0x5412;
            QCOMPARE(bits >> 13, level.second);
            QCOMPARE((bits >> 10) & 7, mask);
            int check = bits >> 10;
            for (int i = 0; i < 10; ++i) check = (check << 1) ^ ((check >> 9) * 0x537);
            QCOMPARE(check & 0x3ff, bits & 0x3ff);
            QVERIFY(matrix.dark(8, matrix.size - 8)); // dark module
        }
    }
}

void TestPlankBroker::qrEncoderCapacityLimits()
{
    QCOMPARE(QrEncoder::byteCapacity(1, QrEncoder::Ecc::Medium), 14);
    QCOMPARE(QrEncoder::byteCapacity(6, QrEncoder::Ecc::Medium), 106);
    QCOMPARE(QrEncoder::byteCapacity(40, QrEncoder::Ecc::Low), 2953);
    QCOMPARE(QrEncoder::encode(QByteArray(14, 'a')).version, 1);
    QCOMPARE(QrEncoder::encode(QByteArray(15, 'a')).version, 2);
    QVERIFY(!QrEncoder::encode(QByteArray(2954, 'a'), QrEncoder::Ecc::Low).isValid());
    QVERIFY(!QrEncoder::encode("x", QrEncoder::Ecc::Medium, 8).isValid());
    QVERIFY(!QrEncoder::encode(QByteArray(20, 'a'), QrEncoder::Ecc::Medium, -1, 1, 1).isValid());
}

void TestPlankBroker::parsesEnrollmentReplies()
{
    using namespace PlankEnrollment;
    Reply reply = parseReply(Endpoint::Start, 200, EnrollStartReply);
    QCOMPARE(reply.state, State::Challenge);
    QCOMPARE(reply.challenge.conversationId, QStringLiteral("en-1"));
    QCOMPARE(reply.challenge.prompts.size(), 1);
    QCOMPARE(reply.challenge.prompts.at(0).style, QStringLiteral("secret"));

    reply = parseReply(Endpoint::Respond, 200, EnrollNewPasswordReply);
    QCOMPARE(reply.state, State::NewPassword);
    QCOMPARE(reply.policy.minLength, 12);
    QCOMPARE(reply.policy.minClasses, 3);

    reply = parseReply(Endpoint::Respond, 200, enrollTotpReply());
    QCOMPARE(reply.state, State::Totp);
    QCOMPARE(reply.secret, QString::fromLatin1(EnrollSecret));
    QVERIFY(reply.otpauthUri.startsWith(QStringLiteral("otpauth://totp/")));

    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"already_enrolled\"}").state, State::AlreadyEnrolled);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"denied\"}").state, State::Denied);

    const QList<QPair<QByteArray, RejectReason>> reasons = {
        {"too_short", RejectReason::TooShort}, {"too_simple", RejectReason::TooSimple},
        {"reused", RejectReason::Reused}, {"policy", RejectReason::Policy}, {"something_new", RejectReason::Policy},
    };
    for (const auto& reason : reasons) {
        reply = parseReply(Endpoint::Password, 200, R"({"state":"password_rejected","reason":")" + reason.first + "\"}");
        QCOMPARE(reply.state, State::PasswordRejected);
        QCOMPARE(reply.reason, reason.second);
    }
    QCOMPARE(parseReply(Endpoint::Password, 200, enrollTotpReply()).state, State::Totp);

    reply = parseReply(Endpoint::Totp, 200, R"({"state":"authenticated","session_token":"sess-1","expires_in":43200,)"
                                            R"("username":"anna","device_bound":true,"passkey_available":true})");
    QCOMPARE(reply.state, State::Authenticated);
    QCOMPARE(reply.session.sessionToken, QStringLiteral("sess-1"));
    QCOMPARE(reply.session.expiresIn, 43200);
    QCOMPARE(reply.session.username, QStringLiteral("anna"));
    QVERIFY(reply.session.deviceBound);
    QVERIFY(reply.passkeyAvailable);
    reply = parseReply(Endpoint::Totp, 200, R"({"state":"enrolled","passkey_available":false})");
    QCOMPARE(reply.state, State::Enrolled);
    QVERIFY(!reply.passkeyAvailable);
    QCOMPARE(parseReply(Endpoint::Totp, 200, "{\"state\":\"code_rejected\"}").state, State::CodeRejected);

    QCOMPARE(parseReply(Endpoint::Passkey, 200, "{\"state\":\"passkey_added\"}").state, State::PasskeyAdded);
    QCOMPARE(parseReply(Endpoint::Passkey, 200, "{\"state\":\"passkey_rejected\"}").state, State::PasskeyRejected);
    QCOMPARE(parseReply(Endpoint::Finish, 200, EnrollDone).state, State::Done);

    reply = parseReply(Endpoint::Respond, 429, R"({"retry_after":90})");
    QCOMPARE(reply.state, State::RateLimited);
    QCOMPARE(reply.retryAfter, 90);
}

void TestPlankBroker::enrollmentUnknownOrMisplacedStatesAreDenied()
{
    using namespace PlankEnrollment;
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"welcome_back\"}").state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{}").state, State::Denied);
    // Valid states, wrong endpoint: never skip a step the Client did not take.
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"authenticated\",\"session_token\":\"t\"}").state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Password, 200, "{\"state\":\"enrolled\"}").state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Totp, 200, enrollTotpReply()).state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Passkey, 200, "{\"state\":\"code_rejected\"}").state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Start, 200, "{\"state\":\"denied\"}").state, State::Denied);
    QCOMPARE(parseReply(Endpoint::Finish, 200, "{\"state\":\"whatever\"}").state, State::Denied);
}

void TestPlankBroker::rejectsMalformedEnrollmentReplies()
{
    using namespace PlankEnrollment;
    QCOMPARE(parseReply(Endpoint::Respond, 500, "{\"state\":\"denied\"}").state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "not json").state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Start, 200, "{\"conversation_id\":\"\",\"prompts\":[{\"id\":\"password\",\"style\":\"secret\"}]}").state,
             State::Malformed);
    QCOMPARE(parseReply(Endpoint::Start, 200, "{\"conversation_id\":\"c\",\"prompts\":[{\"id\":\"x\",\"style\":\"weird\"}]}").state,
             State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"new_password\"}").state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"new_password\",\"policy\":{\"min_length\":-1,\"min_classes\":1}}").state,
             State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"new_password\",\"policy\":{\"min_length\":8.5,\"min_classes\":1}}").state,
             State::Malformed);
    // The typed key and the QR code must describe the same token.
    const QByteArray mismatched = R"({"state":"totp","otpauth_uri":"otpauth://totp/x?secret=AAAAAAAAAAAAAAAA","secret":"JBSWY3DPEHPK3PXP"})";
    QCOMPARE(parseReply(Endpoint::Respond, 200, mismatched).state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, "{\"state\":\"totp\",\"otpauth_uri\":\"https://example.test/\",\"secret\":\"JBSWY3DPEHPK3PXP\"}").state,
             State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, enrollTotpReply("jbswy3dpehpk3pxp")).state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Respond, 200, enrollTotpReply("JBSWY3DP")).state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Totp, 200, "{\"state\":\"authenticated\",\"passkey_available\":true}").state, State::Malformed);
    QCOMPARE(parseReply(Endpoint::Totp, 200, "{\"state\":\"authenticated\",\"session_token\":\"t\",\"passkey_available\":\"yes\"}").state,
             State::Malformed);
    QCOMPARE(parseReply(Endpoint::Totp, 200, "{\"state\":\"enrolled\",\"passkey_available\":1}").state, State::Malformed);
}

void TestPlankBroker::validatesEnrollmentValues()
{
    using namespace PlankEnrollment;
    QVERIFY(isBase32Secret(QString::fromLatin1(EnrollSecret)));
    QVERIFY(!isBase32Secret(QStringLiteral("JBSWY3DPEHPK3PX1")));   // '1' is not base32
    QVERIFY(!isBase32Secret(QStringLiteral("JBSWY3DPEHPK3PXP====")));
    QCOMPARE(groupSecret(QStringLiteral("JBSWY3DPEHPK3PXPJB")), QStringLiteral("JBSW Y3DP EHPK 3PXP JB"));
    QCOMPARE(groupSecret(QString()), QString());
    QVERIFY(isOtpauthUri(QString::fromLatin1(OtpauthVectorUri), QString::fromLatin1(EnrollSecret)));
    QVERIFY(!isOtpauthUri(QStringLiteral("otpauth://hotp/x?secret=JBSWY3DPEHPK3PXP"), QStringLiteral("JBSWY3DPEHPK3PXP")));
    QVERIFY(!isOtpauthUri(QStringLiteral("otpauth://totp/x?secret=JBSWY3DPEHPK3PXP&secret=JBSWY3DPEHPK3PXP"),
                          QStringLiteral("JBSWY3DPEHPK3PXP")));
    QVERIFY(isPasskeyMapping(passkeyMapping()));
    QVERIFY(!isPasskeyMapping(QStringLiteral("passkey:abc")));
    QVERIFY(!isPasskeyMapping(QStringLiteral("key:%1,%2").arg(PasskeyCredential, DeviceSpki)));
    QVERIFY(!isPasskeyMapping(passkeyMapping() + QStringLiteral(",extra")));
    QCOMPARE(path(Endpoint::Start), QStringLiteral("/v1/enroll/start"));
    QCOMPARE(path(Endpoint::Finish), QStringLiteral("/v1/enroll/finish"));
}

void TestPlankBroker::checksNewPasswordsLocally()
{
    using namespace PlankEnrollment;
    const PasswordPolicy policy {12, 3};
    QCOMPARE(checkNewPassword(QString(), QString(), policy), PasswordCheck::Empty);
    QCOMPARE(checkNewPassword(QStringLiteral("Correct-horse-9"), QStringLiteral("Correct-horse-8"), policy),
             PasswordCheck::Mismatch);
    QCOMPARE(checkNewPassword(QStringLiteral("Short-9"), QStringLiteral("Short-9"), policy), PasswordCheck::TooShort);
    QCOMPARE(checkNewPassword(QStringLiteral("alllowercaseletters"), QStringLiteral("alllowercaseletters"), policy),
             PasswordCheck::TooSimple);
    QCOMPARE(checkNewPassword(QStringLiteral("Correct-horse-9"), QStringLiteral("Correct-horse-9"), policy),
             PasswordCheck::Ok);
    QCOMPARE(characterClasses(QStringLiteral("aA1-")), 4);
    QCOMPARE(characterClasses(QString::fromUtf8("aA1-ä")), 5);
    const QString tooLong(MaximumPasswordLength + 1, QLatin1Char('a'));
    QCOMPARE(checkNewPassword(tooLong, tooLong, PasswordPolicy()), PasswordCheck::TooLong);
}

void TestPlankBroker::enrollmentTextsAreGeneric()
{
    using namespace PlankEnrollment;
    const PasswordPolicy policy {12, 3};
    QCOMPARE(noticeText(Notice::Denied, policy),
             QStringLiteral("We couldn't start setup. Check your username and one-time password. "
                            "If it's more than 7 days old, ask the studio for a new one."));
    QCOMPARE(noticeText(Notice::AlreadyEnrolled, policy),
             QStringLiteral("Your account is already set up. Sign in with your password and authenticator code."));
    QVERIFY(noticeText(Notice::None, policy).isEmpty());
    QVERIFY(noticeText(Notice::PasswordTooShort, policy).contains(QStringLiteral("12")));
    QVERIFY(noticeText(Notice::PasswordTooSimple, policy).contains(QStringLiteral("3")));
    QVERIFY(passwordCheckText(PasswordCheck::Ok, policy).isEmpty());
    QCOMPARE(passwordCheckText(PasswordCheck::TooShort, policy), noticeText(Notice::PasswordTooShort, policy));
    // Every text is distinct and none names the account or says it exists.
    const QList<Notice> notices = {
        Notice::Denied, Notice::DeniedAfterPasswordChange, Notice::DeniedPasswordMaybeChanged,
        Notice::AlreadyEnrolled, Notice::PasswordTooShort,
        Notice::PasswordTooSimple, Notice::PasswordReused, Notice::PasswordPolicy, Notice::CodeRejected,
        Notice::NextCodeRejected, Notice::PasskeyNotAdded, Notice::PasskeyNotCreated,
    };
    QSet<QString> texts;
    for (const Notice notice : notices) {
        const QString text = noticeText(notice, policy);
        QVERIFY(!text.isEmpty());
        QVERIFY(!text.contains(QStringLiteral("does not exist"), Qt::CaseInsensitive));
        QVERIFY(!text.contains(QStringLiteral("unknown user"), Qt::CaseInsensitive));
        texts.insert(text);
    }
    QCOMPARE(texts.size(), notices.size());
}

void TestPlankBroker::enrollmentWalkExpiredPasswordToTouchId()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", EnrollNewPasswordReply);
    server.queue("200 OK", R"({"state":"password_rejected","reason":"too_short"})");
    server.queue("200 OK", enrollTotpReply());
    server.queue("200 OK", R"({"state":"code_rejected"})");
    server.queue("200 OK", R"({"state":"authenticated","session_token":"sess-1","expires_in":43200,)"
                           R"("username":"anna","device_bound":false,"passkey_available":true})");
    server.queue("200 OK", R"({"state":"passkey_added"})");
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    Outcome outcome = conversation.begin(QStringLiteral(" anna "), QStringLiteral("one-time-7Qx"));
    QCOMPARE(outcome.step, Step::NewPassword);
    QCOMPARE(outcome.notice, Notice::None);
    QCOMPARE(outcome.policy.minLength, 12);
    QCOMPARE(conversation.username(), QStringLiteral("anna"));

    outcome = conversation.changePassword(QStringLiteral("short"));
    QCOMPARE(outcome.step, Step::NewPassword);
    QCOMPARE(outcome.notice, Notice::PasswordTooShort);
    QCOMPARE(outcome.policy.minLength, 12);

    outcome = conversation.changePassword(QStringLiteral("Correct-horse-9"));
    QCOMPARE(outcome.step, Step::Authenticator);
    QCOMPARE(outcome.secret, QString::fromLatin1(EnrollSecret));
    QVERIFY(isOtpauthUri(outcome.otpauthUri, outcome.secret));

    // A malformed code never reaches the broker.
    outcome = conversation.verifyCode(QStringLiteral("12ab56"));
    QCOMPARE(outcome.step, Step::Authenticator);
    QCOMPARE(outcome.notice, Notice::CodeRejected);
    QCOMPARE(server.requestLog.size(), 4);

    outcome = conversation.verifyCode(QStringLiteral("000000"));
    QCOMPARE(outcome.step, Step::Authenticator);
    QCOMPARE(outcome.notice, Notice::CodeRejected);
    QCOMPARE(outcome.secret, QString::fromLatin1(EnrollSecret)); // the same token, scan again not needed

    outcome = conversation.verifyCode(QStringLiteral("123456"));
    QCOMPARE(outcome.step, Step::Passkey);
    QVERIFY(outcome.signedIn());
    QCOMPARE(outcome.sessionToken, QStringLiteral("sess-1"));
    QCOMPARE(outcome.username, QStringLiteral("anna"));
    QVERIFY(outcome.passkeyAvailable);
    QVERIFY(outcome.secret.isEmpty());

    outcome = conversation.addPasskey(passkeyMapping());
    QCOMPARE(outcome.step, Step::Done);
    QCOMPARE(outcome.notice, Notice::None);
    QVERIFY(!conversation.hasConversation());

    // Exactly the contract's requests, in order.
    QCOMPARE(server.requestLog.size(), 8);
    const QList<QByteArray> targets = {
        "/v1/enroll/start", "/v1/enroll/respond", "/v1/enroll/password", "/v1/enroll/password",
        "/v1/enroll/totp", "/v1/enroll/totp", "/v1/enroll/passkey", "/v1/enroll/finish",
    };
    for (int i = 0; i < targets.size(); ++i) {
        QCOMPARE(requestTarget(server.requestLog.at(i)), targets.at(i));
        QVERIFY(!server.requestLog.at(i).contains("Authorization:"));
    }
    QCOMPARE(requestJson(server.requestLog.at(0)), QJsonObject({{QStringLiteral("username"), QStringLiteral("anna")}}));
    QCOMPARE(requestJson(server.requestLog.at(1)), QJsonObject({
        {QStringLiteral("conversation_id"), QStringLiteral("en-1")},
        {QStringLiteral("responses"), QJsonArray {QStringLiteral("one-time-7Qx")}},
    }));
    QCOMPARE(requestJson(server.requestLog.at(3)), QJsonObject({
        {QStringLiteral("conversation_id"), QStringLiteral("en-1")},
        {QStringLiteral("new_password"), QStringLiteral("Correct-horse-9")},
    }));
    QCOMPARE(requestJson(server.requestLog.at(5)), QJsonObject({
        {QStringLiteral("conversation_id"), QStringLiteral("en-1")},
        {QStringLiteral("code"), QStringLiteral("123456")},
    }));
    QCOMPARE(requestJson(server.requestLog.at(6)), QJsonObject({
        {QStringLiteral("conversation_id"), QStringLiteral("en-1")},
        {QStringLiteral("mapping"), passkeyMapping()},
    }));
    QCOMPARE(requestJson(server.requestLog.at(7)), QJsonObject({{QStringLiteral("conversation_id"), QStringLiteral("en-1")}}));
}

void TestPlankBroker::enrollmentWalkEnrolledThenNextCode()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    // Not expired (changed at a studio desk): straight to the authenticator.
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", enrollTotpReply());
    server.queue("200 OK", R"({"state":"enrolled","passkey_available":false})");
    // The next code, through the normal sign-in: first rejected, then accepted.
    const QByteArray challenge = R"({"state":"challenge","conversation_id":"c1","prompts":[)"
                                 R"({"id":"password","style":"secret","text":"Password"},)"
                                 R"({"id":"otp","style":"otp","text":"Authenticator code"}]})";
    server.queue("200 OK", challenge);
    server.queue("200 OK", R"({"state":"denied"})");
    server.queue("200 OK", challenge);
    server.queue("200 OK", R"({"state":"authenticated","session_token":"sess-2","username":"anna"})");
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    Outcome outcome = conversation.begin(QStringLiteral("anna"), QStringLiteral("Desk-chosen-4"));
    QCOMPARE(outcome.step, Step::Authenticator);
    outcome = conversation.verifyCode(QStringLiteral("111111"));
    QCOMPARE(outcome.step, Step::NextCode);
    QVERIFY(!outcome.signedIn());
    QCOMPARE(outcome.username, QStringLiteral("anna"));
    QVERIFY(outcome.secret.isEmpty());

    outcome = conversation.signInWithNextCode(QStringLiteral("222222"));
    QCOMPARE(outcome.step, Step::NextCode);
    QCOMPARE(outcome.notice, Notice::NextCodeRejected);
    outcome = conversation.signInWithNextCode(QStringLiteral("333333"));
    QCOMPARE(outcome.step, Step::Done);
    QCOMPARE(outcome.sessionToken, QStringLiteral("sess-2"));
    QVERIFY(!conversation.hasConversation());

    QCOMPARE(server.requestLog.size(), 8);
    QCOMPARE(requestTarget(server.requestLog.at(3)), QByteArray("/v1/auth/start"));
    QCOMPARE(requestTarget(server.requestLog.at(6)), QByteArray("/v1/auth/respond"));
    // The kept (current) password and the new code, never the first code.
    QCOMPARE(requestJson(server.requestLog.at(6)).value(QStringLiteral("responses")).toArray(),
             QJsonArray({QStringLiteral("Desk-chosen-4"), QStringLiteral("333333")}));
    QCOMPARE(requestTarget(server.requestLog.at(7)), QByteArray("/v1/enroll/finish"));
}

void TestPlankBroker::enrollmentWalkDeniedAndAlreadyEnrolled()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", R"({"state":"denied"})");
    server.queue("200 OK", EnrollDone);
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", R"({"state":"already_enrolled"})");
    server.queue("200 OK", EnrollDone);
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", R"({"state":"some_future_state"})");
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    Outcome outcome = conversation.begin(QStringLiteral("anna"), QStringLiteral("wrong"));
    QCOMPARE(outcome.step, Step::Credentials);
    QCOMPARE(outcome.notice, Notice::Denied);
    QVERIFY(!conversation.hasConversation());

    outcome = conversation.begin(QStringLiteral("anna"), QStringLiteral("right"));
    QCOMPARE(outcome.step, Step::Credentials);
    QCOMPARE(outcome.notice, Notice::AlreadyEnrolled);

    outcome = conversation.begin(QStringLiteral("anna"), QStringLiteral("right"));
    QCOMPARE(outcome.notice, Notice::Denied);
    QCOMPARE(server.requestLog.size(), 9);

    // Nothing to send without a user name or password.
    outcome = conversation.begin(QStringLiteral("  "), QStringLiteral("x"));
    QCOMPARE(outcome.notice, Notice::Denied);
    QCOMPARE(server.requestLog.size(), 9);
}

void TestPlankBroker::enrollmentWalkDeniedAfterPasswordChange()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", EnrollNewPasswordReply);
    server.queue("200 OK", enrollTotpReply());
    server.queue("200 OK", R"({"state":"denied"})"); // e.g. too many wrong codes
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    QCOMPARE(conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time")).step, Step::NewPassword);
    QCOMPARE(conversation.changePassword(QStringLiteral("Correct-horse-9")).step, Step::Authenticator);
    const Outcome outcome = conversation.verifyCode(QStringLiteral("999999"));
    QCOMPARE(outcome.step, Step::Credentials);
    QCOMPARE(outcome.notice, Notice::DeniedAfterPasswordChange);
    QVERIFY(!outcome.signedIn());
    QCOMPARE(conversation.step(), Step::Credentials);
    // Out-of-order calls are refused locally.
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, conversation.verifyCode(QStringLiteral("123456")));
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, conversation.addPasskey(passkeyMapping()));
    QCOMPARE(server.requestLog.size(), 5);
}

void TestPlankBroker::enrollmentWalkPasskeyRejected()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", enrollTotpReply());
    server.queue("200 OK", R"({"state":"authenticated","session_token":"sess-3","passkey_available":true})");
    server.queue("200 OK", R"({"state":"passkey_rejected"})");
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    conversation.begin(QStringLiteral("anna"), QStringLiteral("Desk-chosen-4"));
    Outcome outcome = conversation.verifyCode(QStringLiteral("123456"));
    QCOMPARE(outcome.step, Step::Passkey);
    QCOMPARE(outcome.username, QStringLiteral("anna")); // no username in the reply: the typed one
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, conversation.addPasskey(QStringLiteral("not a mapping")));
    outcome = conversation.addPasskey(passkeyMapping());
    QCOMPARE(outcome.step, Step::Done);
    QCOMPARE(outcome.notice, Notice::PasskeyNotAdded);
    QCOMPARE(server.requestLog.size(), 5);
}

void TestPlankBroker::enrollmentWalkLostPasswordReply()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    // The broker changes the password but the reply never arrives; the
    // conversation has moved on, so the retry is denied.
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", EnrollNewPasswordReply);
    server.queue(DropReply, QByteArray());
    server.queue("200 OK", R"({"state":"denied"})");
    server.queue("200 OK", EnrollDone);
    // A second run: the reply is lost, but the retry shows nothing changed.
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", EnrollNewPasswordReply);
    server.queue(DropReply, QByteArray());
    server.queue("200 OK", R"({"state":"password_rejected","reason":"too_short"})");
    server.queue("200 OK", R"({"state":"denied"})");
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    QCOMPARE(conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time")).step, Step::NewPassword);
    try {
        conversation.changePassword(QStringLiteral("Correct-horse-9"));
        QFAIL("a lost reply must throw");
    } catch (const PlankBrokerError& error) {
        QVERIFY(error.kind() != PlankBrokerError::RateLimited);
    }
    QCOMPARE(conversation.step(), Step::NewPassword);
    Outcome outcome = conversation.changePassword(QStringLiteral("Correct-horse-9"));
    QCOMPARE(outcome.step, Step::Credentials);
    QCOMPARE(outcome.notice, Notice::DeniedPasswordMaybeChanged);
    const QString text = noticeText(outcome.notice, PasswordPolicy {});
    QVERIFY(text.contains(QStringLiteral("new password")));
    QVERIFY(text != noticeText(Notice::Denied, PasswordPolicy {}));
    QCOMPARE(server.requestLog.size(), 5);

    // A definite "still at the password stage" answer clears the doubt.
    QCOMPARE(conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time")).step, Step::NewPassword);
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, conversation.changePassword(QStringLiteral("Correct-horse-9")));
    outcome = conversation.changePassword(QStringLiteral("Correct-horse-9"));
    QCOMPARE(outcome.step, Step::NewPassword);
    QCOMPARE(outcome.notice, Notice::PasswordTooShort);
    outcome = conversation.changePassword(QStringLiteral("Correct-horse-10"));
    QCOMPARE(outcome.notice, Notice::Denied);
    QCOMPARE(server.requestLog.size(), 11);
}

void TestPlankBroker::enrollmentWalkTransportErrorsKeepTheStep()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("429 Too Many Requests", R"({"retry_after":120})");
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", EnrollNewPasswordReply);
    server.queue("200 OK", R"({"state":"new_password","policy":{}})"); // malformed for /password
    server.queue("200 OK", EnrollDone);

    Conversation conversation(localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)}));
    try {
        conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time"));
        QFAIL("429 must throw");
    } catch (const PlankBrokerError& error) {
        QCOMPARE(error.kind(), PlankBrokerError::RateLimited);
        QCOMPARE(error.retryAfter(), 120);
        QCOMPARE(error.userMessage(), QStringLiteral("Too many attempts. Try again in 120 seconds."));
    }
    QCOMPARE(conversation.step(), Step::Credentials);
    QCOMPARE(conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time")).step, Step::NewPassword);
    // new_password is not an answer /password may give: denied, conversation over.
    const Outcome outcome = conversation.changePassword(QStringLiteral("Correct-horse-9"));
    QCOMPARE(outcome.notice, Notice::Denied);

    // A broker whose key matches no pin never sees the password.
    TestBrokerServer impostor(QSsl::TlsV1_3OrLater);
    QVERIFY(impostor.listen());
    Conversation pinned(localConfig(impostor.port(), {QString::fromLatin1(RsaSpkiSha256)}));
    QVERIFY_THROWS_EXCEPTION(PlankBrokerError, pinned.begin(QStringLiteral("anna"), QStringLiteral("secret-one-time")));
    QCoreApplication::processEvents();
    QVERIFY(!impostor.request.contains("secret-one-time"));
    QVERIFY(!impostor.request.contains("anna"));
}

void TestPlankBroker::enrollmentStartSendsDeviceKey()
{
    using namespace PlankEnrollment;
    TestBrokerServer server(QSsl::TlsV1_3OrLater);
    QVERIFY(server.listen());
    server.queue("200 OK", EnrollStartReply);
    server.queue("200 OK", R"({"state":"denied"})");
    server.queue("200 OK", EnrollDone);
    PlankBrokerClient::Config config = localConfig(server.port(), {QString::fromLatin1(EcSpkiSha256)});
    config.devicePublicKey = []() { return DeviceSpki; };
    Conversation conversation(config);
    conversation.begin(QStringLiteral("anna"), QStringLiteral("one-time"));
    QCOMPARE(requestJson(server.requestLog.at(0)), QJsonObject({
        {QStringLiteral("username"), QStringLiteral("anna")},
        {QStringLiteral("device_key"), DeviceSpki},
    }));
}

void TestPlankBroker::onboardingDecisionPrecedence()
{
    using OnboardingState::Decision;
    OnboardingState::Inputs fresh;
    QCOMPARE(OnboardingState::decide(fresh), Decision::Show);
    QVERIFY(OnboardingState::shouldShow(fresh));

    // Every sign of earlier use keeps the wizard away; the first one (in the
    // documented order) is reported.
    OnboardingState::Inputs used;
    used.completed = true;
    used.brokerConfigured = false;
    used.rememberedSession = true;
    used.localPasskeys = true;
    used.bookmarks = true;
    used.remoteHosts = true;
    const QList<Decision> order = {
        Decision::Completed, Decision::NotConfigured, Decision::RememberedSession,
        Decision::LocalPasskeys, Decision::Bookmarks, Decision::RemoteHosts,
    };
    for (const Decision expected : order) {
        QCOMPARE(OnboardingState::decide(used), expected);
        QVERIFY(!OnboardingState::shouldShow(used));
        switch (expected) {
        case Decision::Completed: used.completed = false; break;
        case Decision::NotConfigured: used.brokerConfigured = true; break;
        case Decision::RememberedSession: used.rememberedSession = false; break;
        case Decision::LocalPasskeys: used.localPasskeys = false; break;
        case Decision::Bookmarks: used.bookmarks = false; break;
        case Decision::RemoteHosts: used.remoteHosts = false; break;
        default: break;
        }
    }
    QCOMPARE(OnboardingState::decide(used), Decision::Show);

    // Each input alone is enough.
    for (int i = 0; i < 5; ++i) {
        OnboardingState::Inputs one;
        (i == 0 ? one.completed : i == 1 ? one.rememberedSession : i == 2 ? one.localPasskeys :
                  i == 3 ? one.bookmarks : one.remoteHosts) = true;
        QVERIFY(!OnboardingState::shouldShow(one));
    }
}

void TestPlankBroker::onboardingReadsSettingsAndPasskeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(file, QSettings::IniFormat);
        OnboardingState::Inputs inputs;
        OnboardingState::readSettings(settings, inputs);
        QVERIFY(!inputs.completed);
        QVERIFY(!inputs.bookmarks);
        QVERIFY(!inputs.remoteHosts);

        // An existing user's settings: a LAN bookmark (ComputerManager format).
        settings.beginWriteArray(QStringLiteral("hosts"));
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("hostname"), QStringLiteral("ws01"));
        settings.endArray();
        OnboardingState::readSettings(settings, inputs);
        QVERIFY(inputs.bookmarks);
        QVERIFY(!OnboardingState::shouldShow(inputs));
    }
    {
        // A remote display setup (RemoteDisplaySetup format) alone.
        QSettings settings(directory.filePath(QStringLiteral("remote.ini")), QSettings::IniFormat);
        RemoteDisplaySetup::Setup setup;
        setup.hostLayout = RemoteDisplaySetup::layoutForChoice(RemoteDisplaySetup::SingleVirtual);
        setup.virtualMode1 = QStringLiteral("1920x1080");
        setup.virtualMode2 = QStringLiteral("1920x1080");
        setup.scalingMode = RemoteDisplaySetup::scalingForChoice(RemoteDisplaySetup::ScaledSpan);
        QVERIFY(RemoteDisplaySetup::save(settings, QStringLiteral("ws01.example.test"), setup));
        OnboardingState::Inputs inputs;
        OnboardingState::readSettings(settings, inputs);
        QVERIFY(inputs.remoteHosts);
        QVERIFY(!inputs.bookmarks);
        QCOMPARE(OnboardingState::decide(inputs), OnboardingState::Decision::RemoteHosts);
        OnboardingState::markCompleted(settings);
        OnboardingState::readSettings(settings, inputs);
        QVERIFY(inputs.completed);
    }
    {
        // A remote user signs in the normal way, then signs out (or the
        // session expires) before any display setup: the Keychain session is
        // gone, no bookmark, no Touch ID key. The sign-in alone must keep
        // the wizard away on the next launch.
        QSettings settings(directory.filePath(QStringLiteral("signin.ini")), QSettings::IniFormat);
        OnboardingState::Inputs inputs;
        OnboardingState::readSettings(settings, inputs);
        QCOMPARE(OnboardingState::decide(inputs), OnboardingState::Decision::Show);
        OnboardingState::recordBrokerSignIn(settings);
        inputs = OnboardingState::Inputs();
        inputs.rememberedSession = false;
        OnboardingState::readSettings(settings, inputs);
        QCOMPARE(OnboardingState::decide(inputs), OnboardingState::Decision::Completed);
    }

    const QString store = directory.filePath(QStringLiteral("passkeys"));
    QVERIFY(!OnboardingState::hasLocalPasskeys(store));
    QVERIFY(!OnboardingState::hasLocalPasskeys(QString()));
    QVERIFY(QDir().mkpath(store + QStringLiteral("/ipa.example.test/anna")));
    QVERIFY(!OnboardingState::hasLocalPasskeys(store));
    QFile key(store + QStringLiteral("/ipa.example.test/anna/0011.json"));
    QVERIFY(key.open(QIODevice::WriteOnly));
    key.write("{}");
    key.close();
    QVERIFY(OnboardingState::hasLocalPasskeys(store));
}

namespace {

RemoteStreamSetup::Setup customStream(int capture, int profile)
{
    RemoteStreamSetup::Setup setup;
    setup.mode = RemoteStreamSetup::Custom;
    setup.captureSource = capture;
    setup.videoProfile = profile;
    return setup;
}

RemoteStreamSetup::Capabilities linuxHost(int flags, const QStringList& modes)
{
    RemoteStreamSetup::Capabilities caps;
    caps.known = true;
    caps.platform = RemoteStreamSetup::LinuxPlatform;
    caps.featureFlags = flags;
    caps.encodingModes = modes;
    return caps;
}

}

void TestPlankBroker::remoteStreamSetupPersistsPerHost()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, QStringLiteral("ws01.example.test")).mode,
             RemoteStreamSetup::Unset);

    RemoteStreamSetup::Setup setup = customStream(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10,
                                                  StreamingPreferences::PLANK_PROFILE_H264_10BIT_444);
    setup.officeBitratesKbps[StreamingPreferences::PLANK_PROFILE_H264_10BIT_444] = 120000;
    setup.internetBitratesKbps[StreamingPreferences::PLANK_PROFILE_H264_10BIT_444] = 30000;
    QVERIFY(RemoteStreamSetup::saveHost(settings, QStringLiteral("WS01.example.test"), setup));

    const RemoteStreamSetup::Setup loaded = RemoteStreamSetup::loadHost(settings, QStringLiteral("ws01.example.test"));
    QCOMPARE(loaded.mode, RemoteStreamSetup::Custom);
    QCOMPARE(loaded.captureSource, int(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10));
    QCOMPARE(loaded.videoProfile, int(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444));
    QCOMPARE(loaded.officeBitratesKbps, setup.officeBitratesKbps);
    QCOMPARE(loaded.internetBitratesKbps, setup.internetBitratesKbps);
    // Kept next to the display setup, so both are forgotten together.
    QVERIFY(settings.contains(QStringLiteral("remote-hosts/ws01.example.test/stream/video-profile")));
    // Other workstations are independent.
    QCOMPARE(RemoteStreamSetup::loadHost(settings, QStringLiteral("ws02.example.test")).mode,
             RemoteStreamSetup::Unset);

    // "Use the defaults" is remembered as such, without stale values.
    RemoteStreamSetup::Setup follow;
    follow.mode = RemoteStreamSetup::FollowDefaults;
    QVERIFY(RemoteStreamSetup::saveHost(settings, QStringLiteral("ws01.example.test"), follow));
    QCOMPARE(RemoteStreamSetup::loadHost(settings, QStringLiteral("ws01.example.test")).mode,
             RemoteStreamSetup::FollowDefaults);
    QVERIFY(!settings.contains(QStringLiteral("remote-hosts/ws01.example.test/stream/video-profile")));
}

void TestPlankBroker::remoteStreamSetupAcceptsNvenc420()
{
    using P = StreamingPreferences;
    // A stream setup saved before the 4:2:0 profiles has nine bitrates per
    // route: it still loads, and the new profiles get their defaults.
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const QString host = QStringLiteral("ws01.example.test");
    const QString group = QStringLiteral("remote-hosts/ws01.example.test/stream/");
    const QVariantList nine =
            P::plankProfileBitratesToVariantList(P::plankDefaultProfileBitrates()).mid(0, 9);
    settings.setValue(group + QStringLiteral("mode"), QStringLiteral("custom"));
    settings.setValue(group + QStringLiteral("capture-source"), int(P::PLANK_CAPTURE_NVFBC_8BIT));
    settings.setValue(group + QStringLiteral("video-profile"), int(P::PLANK_PROFILE_NVENC_HEVC_10BIT_444));
    settings.setValue(group + QStringLiteral("bitrates-office-kbps"), nine);
    settings.setValue(group + QStringLiteral("bitrates-internet-kbps"), nine);
    RemoteStreamSetup::Setup loaded = RemoteStreamSetup::loadHost(settings, host);
    QCOMPARE(loaded.mode, RemoteStreamSetup::Custom);
    QCOMPARE(loaded.internetBitratesKbps.size(), int(P::PLANK_PROFILE_COUNT));
    QCOMPARE(loaded.internetBitratesKbps[P::PLANK_PROFILE_NVENC_H264_8BIT_420], P::PlankH264Yuv420DefaultBitrateKbps);
    QCOMPARE(loaded.internetBitratesKbps[P::PLANK_PROFILE_NVENC_HEVC_10BIT_420], P::PlankHevcYuv420DefaultBitrateKbps);

    // The new profiles round-trip as a remote workstation's choice.
    loaded.videoProfile = P::PLANK_PROFILE_NVENC_HEVC_10BIT_420;
    loaded.internetBitratesKbps[P::PLANK_PROFILE_NVENC_HEVC_10BIT_420] = 20000;
    QVERIFY(RemoteStreamSetup::saveHost(settings, host, loaded));
    const RemoteStreamSetup::Setup again = RemoteStreamSetup::loadHost(settings, host);
    QCOMPARE(again.videoProfile, int(P::PLANK_PROFILE_NVENC_HEVC_10BIT_420));
    QCOMPARE(RemoteStreamSetup::bitrateFor(again, RemoteStreamSetup::Internet), 20000);
    QVERIFY(!RemoteStreamSetup::isValid(customStream(P::PLANK_CAPTURE_X11_NATIVE10,
                                                     P::PLANK_PROFILE_NVENC_H264_8BIT_420)));

    // Host capabilities: advertised modes decide; without a list, the feature bit.
    const int flags = 0x2000 | RemoteStreamSetup::NvfbcNvenc420Feature;
    const QStringList modes{QStringLiteral("hevc-10-444-nvenc"), QStringLiteral("h264-8-420-nvenc"),
                            QStringLiteral("hevc-10-420-nvenc")};
    for (const int profile : {int(P::PLANK_PROFILE_NVENC_H264_8BIT_420), int(P::PLANK_PROFILE_NVENC_HEVC_10BIT_420)}) {
        QVERIFY(!RemoteStreamSetup::profileName(profile).contains(QStringLiteral("unknown")));
        QVERIFY(RemoteStreamSetup::problemFor(P::PLANK_CAPTURE_NVFBC_8BIT, profile, linuxHost(flags, modes)).isEmpty());
        QVERIFY(RemoteStreamSetup::problemFor(P::PLANK_CAPTURE_NVFBC_8BIT, profile, linuxHost(flags, {})).isEmpty());
        QVERIFY(!RemoteStreamSetup::problemFor(P::PLANK_CAPTURE_NVFBC_8BIT, profile,
                                               linuxHost(0x2000, {})).isEmpty());
        QVERIFY(!RemoteStreamSetup::problemFor(P::PLANK_CAPTURE_NVFBC_8BIT, profile,
                                               linuxHost(flags, {QStringLiteral("hevc-10-444-nvenc")})).isEmpty());
    }
}

void TestPlankBroker::remoteStreamSetupRejectsInvalidEntries()
{
    // X11 native 10-bit cannot feed the H.264 NVENC profile.
    QVERIFY(!RemoteStreamSetup::isValid(customStream(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10,
                                                     StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444)));
    QVERIFY(!RemoteStreamSetup::isValid(customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
                                                     StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420)));
    RemoteStreamSetup::Setup tooFast = customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
                                                    StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444);
    tooFast.internetBitratesKbps[0] = StreamingPreferences::PlankBitrateMaximumKbps + 500;
    QVERIFY(!RemoteStreamSetup::isValid(tooFast));

    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const QString host = QStringLiteral("ws01.example.test");
    QVERIFY(!RemoteStreamSetup::saveHost(settings, host, tooFast));
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);

    const QString group = QStringLiteral("remote-hosts/ws01.example.test/stream/");
    const QVariantList bitrates =
            StreamingPreferences::plankProfileBitratesToVariantList(StreamingPreferences::plankDefaultProfileBitrates());
    auto write = [&](int capture, int profile, const QVariant& office, const QVariant& internet) {
        settings.setValue(group + QStringLiteral("mode"), QStringLiteral("custom"));
        settings.setValue(group + QStringLiteral("capture-source"), capture);
        settings.setValue(group + QStringLiteral("video-profile"), profile);
        settings.setValue(group + QStringLiteral("bitrates-office-kbps"), office);
        settings.setValue(group + QStringLiteral("bitrates-internet-kbps"), internet);
    };
    // A valid hand-written entry loads.
    write(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444,
          bitrates, bitrates);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Custom);
    // An unknown profile ID (a newer build's) is treated as unset, never replaced.
    write(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, 42, bitrates, bitrates);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
    // A profile the capture source cannot feed.
    write(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10, StreamingPreferences::PLANK_PROFILE_H264_8BIT_422,
          bitrates, bitrates);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
    // An out-of-range bitrate.
    QVariantList slow = bitrates;
    slow[3] = StreamingPreferences::PlankBitrateMinimumKbps - 500;
    write(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
          bitrates, slow);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
    // Garbage and missing lists.
    write(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
          QStringLiteral("fast"), bitrates);
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
    settings.remove(group + QStringLiteral("bitrates-office-kbps"));
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
    // Profile IDs are append-only: an older build's shorter list gains defaults.
    const QVariantList older = bitrates.mid(0, 7);
    write(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
          older, older);
    const RemoteStreamSetup::Setup migrated = RemoteStreamSetup::loadHost(settings, host);
    QCOMPARE(migrated.mode, RemoteStreamSetup::Custom);
    QCOMPARE(migrated.officeBitratesKbps.size(), int(StreamingPreferences::PLANK_PROFILE_COUNT));
    // Unknown mode strings are unset.
    settings.setValue(group + QStringLiteral("mode"), QStringLiteral("turbo"));
    QCOMPARE(RemoteStreamSetup::loadHost(settings, host).mode, RemoteStreamSetup::Unset);
}

void TestPlankBroker::remoteStreamSetupPicksBitrateForRoute()
{
    RemoteStreamSetup::Setup setup = customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
                                                  StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444);
    setup.officeBitratesKbps[StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444] = 100000;
    setup.internetBitratesKbps[StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444] = 30000;
    QCOMPARE(RemoteStreamSetup::bitrateFor(setup, RemoteStreamSetup::OfficeNetwork), 100000);
    QCOMPARE(RemoteStreamSetup::bitrateFor(setup, RemoteStreamSetup::Internet), 30000);
    QCOMPARE(RemoteStreamSetup::bitratesFor(setup, RemoteStreamSetup::Internet), setup.internetBitratesKbps);
    // Each profile keeps its own target per route.
    setup.videoProfile = StreamingPreferences::PLANK_PROFILE_H264_10BIT_444;
    QCOMPARE(RemoteStreamSetup::bitrateFor(setup, RemoteStreamSetup::Internet),
             StreamingPreferences::PlankH264DefaultBitrateKbps);
    // Built-in: NVENC HEVC 10-bit 4:4:4 at 50 Mbps on both routes.
    const RemoteStreamSetup::Setup builtIn = RemoteStreamSetup::builtInDefaults(RemoteStreamSetup::LinuxPlatform);
    QCOMPARE(builtIn.videoProfile, int(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444));
    QCOMPARE(builtIn.captureSource, int(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT));
    QCOMPARE(RemoteStreamSetup::bitrateFor(builtIn, RemoteStreamSetup::OfficeNetwork), 50000);
    QCOMPARE(RemoteStreamSetup::bitrateFor(builtIn, RemoteStreamSetup::Internet), 50000);
}

void TestPlankBroker::remoteStreamSetupLayersDefaults()
{
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    QCOMPARE(RemoteStreamSetup::loadDefaults(settings).mode, RemoteStreamSetup::Unset);
    RemoteStreamSetup::Setup defaults = customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
                                                     StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444);
    defaults.internetBitratesKbps[StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444] = 25000;
    QVERIFY(RemoteStreamSetup::saveDefaults(settings, defaults));
    defaults = RemoteStreamSetup::loadDefaults(settings);
    QCOMPARE(defaults.mode, RemoteStreamSetup::Custom);
    QCOMPARE(defaults.videoProfile, int(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444));
    QCOMPARE(RemoteStreamSetup::bitrateFor(defaults, RemoteStreamSetup::Internet), 25000);

    const RemoteStreamSetup::Setup unset;
    RemoteStreamSetup::Setup follow;
    follow.mode = RemoteStreamSetup::FollowDefaults;
    const RemoteStreamSetup::Setup host = customStream(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10,
                                                       StreamingPreferences::PLANK_PROFILE_H264_10BIT_444);
    RemoteStreamSetup::Setup seed = customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
                                                 StreamingPreferences::PLANK_PROFILE_H264_8BIT_444);
    const int linuxHostPlatform = RemoteStreamSetup::LinuxPlatform;

    // Per workstation > bookmark seed > remote access defaults > built-in.
    RemoteStreamSetup::Resolution r = RemoteStreamSetup::resolve(host, &seed, defaults, linuxHostPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromHost);
    QCOMPARE(r.setup.videoProfile, int(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444));
    r = RemoteStreamSetup::resolve(unset, &seed, defaults, linuxHostPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromBookmark);
    QCOMPARE(r.setup.videoProfile, int(StreamingPreferences::PLANK_PROFILE_H264_8BIT_444));
    // Choosing "use the defaults" is never overridden by a bookmark.
    r = RemoteStreamSetup::resolve(follow, &seed, defaults, linuxHostPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromDefaults);
    QCOMPARE(r.setup.videoProfile, int(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444));
    r = RemoteStreamSetup::resolve(unset, nullptr, defaults, RemoteStreamSetup::UnknownPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromDefaults);
    r = RemoteStreamSetup::resolve(unset, nullptr, RemoteStreamSetup::Setup(), linuxHostPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromBuiltIn);
    QCOMPARE(r.setup.videoProfile, int(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444));

    // A remote Mac skips Linux layers and gets its own built-in default.
    r = RemoteStreamSetup::resolve(unset, &seed, defaults, RemoteStreamSetup::MacPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromBuiltIn);
    QCOMPARE(r.setup.captureSource, int(StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT));
    QCOMPARE(r.setup.videoProfile, int(StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420));
    // ...but the user's own choice is kept (and reported, see below).
    r = RemoteStreamSetup::resolve(host, nullptr, defaults, RemoteStreamSetup::MacPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromHost);
    QCOMPARE(r.setup.captureSource, int(StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10));

    // Restoring the defaults drops the saved layer.
    RemoteStreamSetup::clearDefaults(settings);
    QCOMPARE(RemoteStreamSetup::loadDefaults(settings).mode, RemoteStreamSetup::Unset);
    QVERIFY(!RemoteStreamSetup::saveDefaults(settings, customStream(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT, 99)));
}

void TestPlankBroker::remoteStreamSetupSeedsOnlyFromExactBookmark()
{
    auto bookmark = [](const QString& name, const QString& address, int profile) {
        RemoteStreamSetup::BookmarkCandidate candidate;
        candidate.name = name;
        candidate.address = address;
        candidate.captureSource = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
        candidate.videoProfile = profile;
        candidate.bitratesKbps = StreamingPreferences::plankDefaultProfileBitrates();
        candidate.bitratesKbps[profile] = 90000;
        return candidate;
    };
    const QString hostId = QStringLiteral("ws01.example.test");
    const QString hostName = QStringLiteral("ws01");
    RemoteStreamSetup::Setup seed;

    // Same name, or an address equal to the host id or name, seeds.
    QVERIFY(RemoteStreamSetup::seedFromBookmarks(
                {bookmark(QStringLiteral("WS01"), QStringLiteral("192.0.2.10"),
                          StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444)}, hostId, hostName, seed));
    QCOMPARE(seed.videoProfile, int(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444));
    // A LAN bookmark's target is for the office network only.
    QCOMPARE(RemoteStreamSetup::bitrateFor(seed, RemoteStreamSetup::OfficeNetwork), 90000);
    QCOMPARE(RemoteStreamSetup::bitrateFor(seed, RemoteStreamSetup::Internet),
             StreamingPreferences::PlankHevcDefaultBitrateKbps);
    QVERIFY(RemoteStreamSetup::seedFromBookmarks(
                {bookmark(QStringLiteral("Grading"), QStringLiteral("WS01.example.test"),
                          StreamingPreferences::PLANK_PROFILE_H264_8BIT_444)}, hostId, hostName, seed));
    QCOMPARE(seed.videoProfile, int(StreamingPreferences::PLANK_PROFILE_H264_8BIT_444));

    // Prefix and look-alike matches do not.
    RemoteStreamSetup::Setup untouched;
    QVERIFY(!RemoteStreamSetup::seedFromBookmarks(
                {bookmark(QStringLiteral("ws01-old"), QStringLiteral("ws01.other.test"),
                          StreamingPreferences::PLANK_PROFILE_H264_8BIT_444),
                 bookmark(QStringLiteral("ws010"), QStringLiteral("192.0.2.11"),
                          StreamingPreferences::PLANK_PROFILE_H264_8BIT_444)}, hostId, hostName, untouched));
    QCOMPARE(untouched.mode, RemoteStreamSetup::Unset);
    // Two exact matches that disagree seed nothing.
    QVERIFY(!RemoteStreamSetup::seedFromBookmarks(
                {bookmark(QStringLiteral("ws01"), QString(), StreamingPreferences::PLANK_PROFILE_H264_8BIT_444),
                 bookmark(QStringLiteral("Other"), hostId, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444)},
                hostId, hostName, untouched));
    // An unusable bookmark is ignored.
    RemoteStreamSetup::BookmarkCandidate broken = bookmark(hostName, QString(),
                                                           StreamingPreferences::PLANK_PROFILE_H264_8BIT_444);
    broken.captureSource = StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10;
    QVERIFY(!RemoteStreamSetup::seedFromBookmarks({broken}, hostId, hostName, untouched));
}

void TestPlankBroker::remoteStreamSetupReportsUnusableChoice()
{
    const int nvfbc = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
    const int native10 = StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10;
    const int sck = StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    const int hevc10 = StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444;
    const QStringList allLinuxModes = RemoteStreamSetup::parseEncodingModes(
                QStringLiteral("h264-8-422-software,h264-8-444-software,h264-10-422-software,"
                               "h264-10-444-software,h264-8-444-nvenc,hevc-8-444-nvenc,hevc-10-444-nvenc,"
                               "h264-8-420-nvenc,hevc-10-420-nvenc"));
    QCOMPARE(allLinuxModes.size(), 9);

    // Nothing known yet: only the pairing is checked.
    const RemoteStreamSetup::Capabilities unknown;
    QVERIFY(RemoteStreamSetup::problemFor(nvfbc, hevc10, unknown).isEmpty());
    QVERIFY(!RemoteStreamSetup::problemFor(native10, StreamingPreferences::PLANK_PROFILE_H264_8BIT_422,
                                           unknown).isEmpty());

    const RemoteStreamSetup::Capabilities full = linuxHost(
                RemoteStreamSetup::NvfbcHevc10NvencFeature | RemoteStreamSetup::NvfbcNvenc420Feature, allLinuxModes);
    for (int profile = 0; profile < StreamingPreferences::PLANK_PROFILE_COUNT; ++profile) {
        if (StreamingPreferences::isPlankAppleProfile(profile)) continue;
        QVERIFY2(RemoteStreamSetup::problemFor(nvfbc, profile, full).isEmpty(), qPrintable(QString::number(profile)));
    }
    // A host from before the 4:2:0 modes (1.0.127) offers everything but those.
    const RemoteStreamSetup::Capabilities older = linuxHost(
                RemoteStreamSetup::NvfbcHevc10NvencFeature, allLinuxModes.mid(0, 7));
    for (int profile = 0; profile < StreamingPreferences::PLANK_PROFILE_COUNT; ++profile) {
        if (StreamingPreferences::isPlankAppleProfile(profile)) continue;
        QCOMPARE(RemoteStreamSetup::problemFor(nvfbc, profile, older).isEmpty(),
                 !StreamingPreferences::isPlankNvenc420Profile(profile));
    }
    // HEVC 10-bit from NvFBC needs the host feature; native 10-bit capture does not.
    const RemoteStreamSetup::Capabilities noHevc10Fbc = linuxHost(0, allLinuxModes);
    const QString hevcProblem = RemoteStreamSetup::problemFor(nvfbc, hevc10, noHevc10Fbc);
    QVERIFY(hevcProblem.contains(QStringLiteral("NvFBC")));
    QVERIFY(RemoteStreamSetup::problemFor(native10, hevc10, noHevc10Fbc).isEmpty());
    // A mode the host does not advertise (e.g. no NVENC).
    const RemoteStreamSetup::Capabilities softwareOnly = linuxHost(
                RemoteStreamSetup::NvfbcHevc10NvencFeature, allLinuxModes.mid(0, 4));
    QVERIFY(RemoteStreamSetup::problemFor(nvfbc, hevc10, softwareOnly).contains(QStringLiteral("can't use")));
    QVERIFY(RemoteStreamSetup::problemFor(nvfbc, StreamingPreferences::PLANK_PROFILE_H264_10BIT_444,
                                          softwareOnly).isEmpty());
    // A host that does not advertise modes is not second-guessed.
    QVERIFY(RemoteStreamSetup::problemFor(nvfbc, StreamingPreferences::PLANK_PROFILE_H264_8BIT_422,
                                          linuxHost(0, {})).isEmpty());
    // Platform mismatches.
    QVERIFY(!RemoteStreamSetup::problemFor(sck, StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420,
                                           full).isEmpty());
    RemoteStreamSetup::Capabilities mac;
    mac.known = true;
    mac.platform = RemoteStreamSetup::MacPlatform;
    QVERIFY(!RemoteStreamSetup::problemFor(nvfbc, hevc10, mac).isEmpty());
    QVERIFY(RemoteStreamSetup::problemFor(sck, StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444,
                                          mac).isEmpty());

    // No silent fallback: a saved choice the workstation cannot use resolves
    // to that choice, and the caller gets a reason to ask the user.
    const RemoteStreamSetup::Setup saved = customStream(nvfbc, hevc10);
    const RemoteStreamSetup::Resolution r = RemoteStreamSetup::resolve(saved, nullptr, RemoteStreamSetup::Setup(),
                                                                       RemoteStreamSetup::LinuxPlatform);
    QCOMPARE(r.source, RemoteStreamSetup::FromHost);
    QCOMPARE(r.setup.videoProfile, hevc10);
    QVERIFY(!RemoteStreamSetup::problemFor(r.setup, noHevc10Fbc).isEmpty());
    // The built-in default is reported too rather than swapped for another.
    const RemoteStreamSetup::Resolution builtIn = RemoteStreamSetup::resolve(
                RemoteStreamSetup::Setup(), nullptr, RemoteStreamSetup::Setup(), RemoteStreamSetup::LinuxPlatform);
    QVERIFY(!RemoteStreamSetup::problemFor(builtIn.setup, noHevc10Fbc).isEmpty());
}

void TestPlankBroker::remoteStreamSetupCachesCapabilities()
{
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const QString host = QStringLiteral("WS01.example.test");
    QVERIFY(!RemoteStreamSetup::loadCapabilities(settings, host).known);
    const RemoteStreamSetup::Capabilities caps = linuxHost(
                RemoteStreamSetup::NvfbcHevc10NvencFeature,
                RemoteStreamSetup::parseEncodingModes(QStringLiteral(" hevc-10-444-nvenc, h264-10-444-software,,hevc-10-444-nvenc")));
    QCOMPARE(caps.encodingModes, QStringList({QStringLiteral("hevc-10-444-nvenc"),
                                              QStringLiteral("h264-10-444-software")}));
    RemoteStreamSetup::saveCapabilities(settings, host, caps);
    const RemoteStreamSetup::Capabilities loaded = RemoteStreamSetup::loadCapabilities(settings, host.toLower());
    QVERIFY(loaded.known);
    QCOMPARE(loaded.platform, int(RemoteStreamSetup::LinuxPlatform));
    QCOMPARE(loaded.featureFlags, RemoteStreamSetup::NvfbcHevc10NvencFeature);
    QCOMPARE(loaded.encodingModes, caps.encodingModes);
    // Unknown capabilities are never written.
    RemoteStreamSetup::saveCapabilities(settings, QStringLiteral("ws02.example.test"), RemoteStreamSetup::Capabilities());
    QVERIFY(!RemoteStreamSetup::loadCapabilities(settings, QStringLiteral("ws02.example.test")).known);
    // The host-side mode names match what launch requests send.
    for (int profile = 0; profile < StreamingPreferences::PLANK_PROFILE_COUNT; ++profile) {
        QVERIFY(!StreamingPreferences::plankEncodingMode(profile).isEmpty());
    }
    QCOMPARE(StreamingPreferences::plankEncodingMode(StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444),
             QStringLiteral("hevc-10-444-nvenc"));
}

void TestPlankBroker::remoteStreamSetupCachesDisplayCapabilities()
{
    QTemporaryDir dir;
    QSettings settings(dir.filePath(QStringLiteral("client.ini")), QSettings::IniFormat);
    const QString host = QStringLiteral("ws01.example.test");
    RemoteStreamSetup::Capabilities caps = linuxHost(0, {});
    caps.displayCapabilities = QStringLiteral(R"({"version":1})");
    RemoteStreamSetup::saveCapabilities(settings, host, caps);
    QCOMPARE(RemoteStreamSetup::loadCapabilities(settings, host).displayCapabilities, caps.displayCapabilities);
    QCOMPARE(settings.value(QStringLiteral("remote-hosts/ws01.example.test/host/display-caps")).toString(),
             caps.displayCapabilities);
    // A connect that failed before the topology was read keeps the cache...
    RemoteStreamSetup::Capabilities early = linuxHost(RemoteStreamSetup::DisplayArrangementFeature, {});
    RemoteStreamSetup::saveCapabilities(settings, host, early);
    QCOMPARE(RemoteStreamSetup::loadCapabilities(settings, host).displayCapabilities, caps.displayCapabilities);
    // ...a host that stops publishing them (downgraded) forgets it.
    caps.displayCapabilities.clear();
    RemoteStreamSetup::saveCapabilities(settings, host, caps);
    QVERIFY(RemoteStreamSetup::loadCapabilities(settings, host).displayCapabilities.isEmpty());
    QVERIFY(!settings.contains(QStringLiteral("remote-hosts/ws01.example.test/host/display-caps")));
}

QTEST_GUILESS_MAIN(TestPlankBroker)
#include "test_plankbroker.moc"

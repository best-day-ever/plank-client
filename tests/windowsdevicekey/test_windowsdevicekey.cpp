#include "backend/brokerdevicekey.h"
#include "backend/brokersessionstore.h"
#include "backend/brokersessionstate.h"
#include "backend/plankenrollment.h"

#include <QtTest>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QFile>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUuid>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/x509.h>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <ncrypt.h>
#endif

using Signature = PlankBrokerClient::DeviceSignature;
using Device = BrokerDeviceKey::WindowsKey;

namespace {
bool verify(const QByteArray& spki, const QByteArray& message, const QByteArray& der)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(spki.constData());
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &bytes, spki.size());
    if (!key) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    const bool valid = context && bytes == reinterpret_cast<const unsigned char*>(spki.constData()) + spki.size() &&
            EVP_PKEY_base_id(key) == EVP_PKEY_EC && EVP_PKEY_bits(key) == 256 &&
            EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
            EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(der.constData()), der.size(),
                             reinterpret_cast<const unsigned char*>(message.constData()), message.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}

QString uniqueAuthority()
{
    return QStringLiteral("device-test-%1.example.invalid:443").arg(QUuid::createUuid().toString(QUuid::Id128));
}

#ifdef Q_OS_WIN
struct DisposableKey {
    QString authority = uniqueAuthority();
    ~DisposableKey() {
        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE key = 0;
        const auto name = Device::keyName(authority);
        if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS) {
            if (NCryptOpenKey(provider, &key, reinterpret_cast<const wchar_t*>(name.utf16()), 0,
                              NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) NCryptDeleteKey(key, NCRYPT_SILENT_FLAG);
            NCryptFreeObject(provider);
        }
    }
};
#endif
}

class TestWindowsDeviceKey : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("PLANK-Device-Tests");
        QCoreApplication::setApplicationName("Disposable");
        QStandardPaths::setTestModeEnabled(true);
    }

    void logoutSnapshotRetainsBoundProofAfterSharedStateClears() {
        auto shared = std::make_shared<BrokerSessionState>();
        shared->set(QStringLiteral("bound-session-token"), true);
        const auto live = BrokerDeviceKey::clientConfig("broker.example.invalid", 443, {QString(64, 'a')},
                                [shared]() { return shared->proofRequired(); }, {});
        // Exercise the actual production Config factory + logout snapshot,
        // then clear precisely the shared state the live callback reads.
        auto frozen = live.withFrozenDeviceProofRequirement();
        const QString token = shared->get();
        shared->clear();
        QVERIFY(!live.deviceProofRequired());
        QVERIFY(frozen.deviceProofRequired());
        frozen.deviceSigner = [](const QByteArray&) { return Signature {Signature::NoKey, {}}; };
        try {
            PlankBrokerClient(frozen).logout(token);
            QFAIL("Bound logout silently sent without a proof");
        } catch (const PlankBrokerError& error) {
            // A Network error would expose an unsigned request to the fake
            // authority. The missing key must stop it before any network I/O.
            QCOMPARE(error.kind(), PlankBrokerError::DeviceKeyUnavailable);
        }
        QVERIFY(!shared->hasToken()); // explicit local logout stays complete
        shared->set(QStringLiteral("new-session-token"), false);
        QVERIFY(frozen.deviceProofRequired()); // later auth cannot alter it
        QVERIFY(!live.withFrozenDeviceProofRequirement().deviceProofRequired());
    }

#ifdef Q_OS_WIN
    void actualClientConfigSupportsIpv4Broker() {
        const auto config = BrokerDeviceKey::clientConfig(QStringLiteral("127.0.0.1"), 443,
                                {QString(64, 'a')}, [] { return true; }, {});
        QVERIFY(config.requireDeviceBinding);
        QVERIFY(config.devicePublicKey);
        QVERIFY(config.deviceSigner);
        QVERIFY(config.deviceProofRequired());
        // Factory inspection creates no key in this fixed authority's store.
        // Native persistence/signing is exercised with disposable names below.
    }
#endif

    void wireEncodingIsCanonical() {
        QCOMPARE(BrokerDeviceKey::p256Spki(QByteArray(65, '\0')), QByteArray());
        QCOMPARE(BrokerDeviceKey::p256Spki(QByteArray(1, '\x04') + QByteArray(64, '\0')), QByteArray());
        QVERIFY(BrokerDeviceKey::ecdsaDer(QByteArray(63, '\1')).isEmpty());
        QVERIFY(BrokerDeviceKey::ecdsaDer(QByteArray(64, '\0')).isEmpty());
        QByteArray raw(64, '\0'); raw[31] = '\x01'; raw[63] = '\x02';
        QCOMPARE(BrokerDeviceKey::ecdsaDer(raw), QByteArray::fromHex("3006020101020102"));
        raw[0] = char(0x80);
        const auto encoded = BrokerDeviceKey::ecdsaDer(raw);
        QCOMPARE(encoded.mid(2, 3), QByteArray::fromHex("022100"));
    }

    void savedMetadataAndLegacyMigration() {
        BrokerSessionStore::Saved saved;
        QVERIFY(BrokerSessionStore::decode(R"({"v":1,"user":"artist","token":"old-grant"})", saved));
        QVERIFY(saved.legacy); QVERIFY(!saved.deviceBound);
#ifdef Q_OS_WIN
        QVERIFY(BrokerSessionStore::needsDeviceSignIn(saved));
#endif
        QVERIFY(BrokerSessionStore::decode(BrokerSessionStore::encode("artist", "new-grant", true), saved));
        QVERIFY(saved.deviceBound); QVERIFY(!saved.legacy);
        QVERIFY(!BrokerSessionStore::needsDeviceSignIn(saved));
        QVERIFY(!BrokerSessionStore::decode(R"({"v":2,"user":"artist","token":"t","device_bound":"true"})", saved));
        QVERIFY(saved.token.isEmpty());
        const auto object = QJsonDocument::fromJson(BrokerSessionStore::encode("artist", "t", true)).object();
        QCOMPARE(object.keys(), QStringList({"device_bound", "token", "user", "v"}));
    }

    void independentOpenSslVerifiesWireEncoding() {
        EVP_PKEY_CTX* generation = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        QVERIFY(generation);
        EVP_PKEY* key = nullptr;
        const bool generated = EVP_PKEY_keygen_init(generation) == 1 &&
                EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generation, NID_X9_62_prime256v1) == 1 &&
                EVP_PKEY_keygen(generation, &key) == 1;
        EVP_PKEY_CTX_free(generation);
        QVERIFY(generated);
        const QByteArray message("existing broker ES256 message");
        EVP_MD_CTX* signing = EVP_MD_CTX_new();
        QVERIFY(signing);
        size_t size = 0;
        QVERIFY(EVP_DigestSignInit(signing, nullptr, EVP_sha256(), nullptr, key) == 1);
        QVERIFY(EVP_DigestSign(signing, nullptr, &size,
                             reinterpret_cast<const unsigned char*>(message.constData()), message.size()) == 1);
        QByteArray der(int(size), '\0');
        const bool signedOk = EVP_DigestSign(signing, reinterpret_cast<unsigned char*>(der.data()), &size,
                              reinterpret_cast<const unsigned char*>(message.constData()), message.size()) == 1;
        EVP_MD_CTX_free(signing);
        QVERIFY(signedOk); der.resize(int(size));
        QByteArray spki(i2d_PUBKEY(key, nullptr), '\0');
        auto* output = reinterpret_cast<unsigned char*>(spki.data());
        i2d_PUBKEY(key, &output);
        EVP_PKEY_free(key);
        const auto* input = reinterpret_cast<const unsigned char*>(der.constData());
        ECDSA_SIG* signature = d2i_ECDSA_SIG(nullptr, &input, der.size());
        QVERIFY(signature);
        const BIGNUM *r = nullptr, *s = nullptr;
        ECDSA_SIG_get0(signature, &r, &s);
        QByteArray raw(64, '\0');
        QCOMPARE(BN_bn2binpad(r, reinterpret_cast<unsigned char*>(raw.data()), 32), 32);
        QCOMPARE(BN_bn2binpad(s, reinterpret_cast<unsigned char*>(raw.data()) + 32, 32), 32);
        ECDSA_SIG_free(signature);
        QCOMPARE(BrokerDeviceKey::ecdsaDer(raw), der);
        QCOMPARE(BrokerDeviceKey::p256Spki(spki.right(65)), spki);
        QVERIFY(verify(spki, message, BrokerDeviceKey::ecdsaDer(raw)));
        QVERIFY(!verify(spki, message + "tampered", der));
    }

    void usernameSurvivesCredentialRemovalAndIsBrokerScoped() {
        const auto authority = uniqueAuthority();
        BrokerSessionStore::rememberUsername(authority, "confirmed-artist");
        QCOMPARE(BrokerSessionStore::rememberedUsername(authority), QString("confirmed-artist"));
        QVERIFY(BrokerSessionStore::rememberedUsername(authority + "-other").isEmpty());
#ifdef Q_OS_WIN
        if (BrokerSessionStore::isAvailable()) {
            QVERIFY(BrokerSessionStore::save(authority, "confirmed-artist", "token", true));
            BrokerSessionStore::Saved saved;
            QVERIFY(BrokerSessionStore::load(authority, saved));
            QVERIFY(saved.deviceBound); QVERIFY(!saved.legacy);
            QProcess child;
            child.start(QCoreApplication::applicationFilePath(), {"--resume-probe", authority});
            QVERIFY(child.waitForFinished(10000));
            QCOMPARE(child.exitCode(), 0);
            QVERIFY(QJsonDocument::fromJson(child.readAllStandardOutput()).object().value("restored").toBool());
            BrokerSessionStore::clear(authority);
            QVERIFY(!BrokerSessionStore::load(authority, saved));
            QVERIFY(BrokerSessionStore::save(authority, "confirmed-artist", "legacy-unbound", false));
            QVERIFY(BrokerSessionStore::load(authority, saved));
            QVERIFY(BrokerSessionStore::needsDeviceSignIn(saved));
            BrokerSessionStore::clear(authority);
        }
#endif
        QCOMPARE(BrokerSessionStore::rememberedUsername(authority), QString("confirmed-artist"));
        QSettings().remove(BrokerSessionStore::usernameKey(authority));
    }

    void boundGrantNeverFallsBackToUnsigned() {
        auto config = BrokerDeviceKey::clientConfig("broker.example.invalid", 443, {QString(64, 'a')}, [] { return true; }, {});
        config.deviceSigner = [](const QByteArray&) { return Signature{Signature::NoKey, {}}; };
        try {
            PlankBrokerClient(config).hosts("bound-token");
            QFAIL("a bound grant without its key must not make an unsigned request");
        } catch (const PlankBrokerError& error) {
            QCOMPARE(error.kind(), PlankBrokerError::DeviceKeyUnavailable);
        }
    }

    void requiredBindingNeverStartsAnUnboundConversation() {
        auto config = BrokerDeviceKey::clientConfig("broker.example.invalid", 443, {QString(64, 'a')}, {}, {});
        config.requireDeviceBinding = true;
        config.devicePublicKey = {};
        try {
            PlankBrokerClient(config).start("artist");
            QFAIL("required binding cannot silently start an unbound sign-in");
        } catch (const PlankBrokerError& error) {
            QCOMPARE(error.kind(), PlankBrokerError::DeviceKeyUnavailable);
        }
        PlankEnrollment::Conversation enrollment(config);
        try {
            enrollment.begin("artist", "disposable-password");
            QFAIL("enrollment must use the same device-binding requirement");
        } catch (const PlankBrokerError& error) {
            QCOMPARE(error.kind(), PlankBrokerError::DeviceKeyUnavailable);
        }
        QVERIFY(!enrollment.hasConversation());
    }

    void usernamePrefillPreservesTypedEdits() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/gui/BrokerUsernameField.qml"));
        QScopedPointer<QObject> field(component.create());
        QVERIFY2(field, qPrintable(component.errorString()));
        field->setProperty("rememberedUsername", "confirmed-artist");
        QVERIFY(QMetaObject::invokeMethod(field.data(), "prefill", Q_ARG(QVariant, QVariant(""))));
        QCOMPARE(field->property("text").toString(), QString("confirmed-artist"));
        field->setProperty("text", "edited-artist");
        QVERIFY(QMetaObject::invokeMethod(field.data(), "prefill", Q_ARG(QVariant, QVariant("onboarding-artist"))));
        QCOMPARE(field->property("text").toString(), QString("edited-artist"));
        field->setProperty("text", "");
        QVERIFY(QMetaObject::invokeMethod(field.data(), "prefill", Q_ARG(QVariant, QVariant("onboarding-artist"))));
        QCOMPARE(field->property("text").toString(), QString("onboarding-artist"));
    }

    void sessionReasonRemainsVisibleUntilAcknowledged() {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/gui/BrokerSessionBanner.qml"));
        QScopedPointer<QObject> banner(component.create());
        QVERIFY2(banner, qPrintable(component.errorString()));
        QVERIFY(!banner->property("visible").toBool());
        banner->setProperty("width", 700);
        banner->setProperty("message", "The saved session was revoked. Sign in again.");
        auto* item = qobject_cast<QQuickItem*>(banner.data());
        QVERIFY(item);
        QQuickWindow window;
        item->setParentItem(window.contentItem());
        window.resize(720, 200);
        item->setHeight(100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(banner->property("visible").toBool());
        QObject* label = nullptr;
        for (auto* row : item->childItems()) for (auto* child : row->childItems())
            if (child->objectName() == "brokerSessionNotice") label = child;
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(), banner->property("message").toString());
        banner->setProperty("message", "");
        QVERIFY(!banner->property("visible").toBool());
        item->setParentItem(nullptr);
    }

#ifdef Q_OS_WIN
    void actualClientConfigUsesNativeKeyWithoutPasskeyHelper() {
        DisposableKey fixture;
        const QString host = fixture.authority.section(':', 0, 0);
        bool bound = false;
        const auto config = BrokerDeviceKey::clientConfig(host, 443, {QString(64, 'a')}, [&] { return bound; }, {});
        QVERIFY(config.devicePublicKey); QVERIFY(config.deviceSigner);
        QVERIFY(config.requireDeviceBinding);
        QVERIFY(!config.deviceProofRequired());
        const QByteArray spki = QByteArray::fromBase64(config.devicePublicKey().toLatin1());
        QCOMPARE(spki.size(), 91);
        bound = true; QVERIFY(config.deviceProofRequired());
        const auto message = PlankBroker::deviceProofMessage("POST", "/v1/lease/fixture/keepalive", 2000000000, "fixture-token", "{}");
        const auto signature = config.deviceSigner(message);
        QCOMPARE(signature.status, Signature::Signed);
        const auto der = QByteArray::fromBase64(signature.signature.toLatin1());
        QVERIFY(verify(spki, message, der));
        QVERIFY(!verify(spki, message + "tampered", der));
        QVERIFY(!verify(spki, message, der.left(der.size() - 1)));
        const QString fixturePath = qEnvironmentVariable("PLANK_DEVICE_PROOF_FIXTURE");
        if (!fixturePath.isEmpty()) {
            QFile output(fixturePath);
            QVERIFY(output.open(QIODevice::WriteOnly));
            const auto bytes = QJsonDocument(QJsonObject{{"public_key", QString::fromLatin1(spki.toBase64())},
                {"message", QString::fromLatin1(message.toBase64())}, {"signature", signature.signature}}).toJson();
            QCOMPARE(output.write(bytes), qint64(bytes.size()));
        }
    }

    void persistedKeyReopensAcrossProcessesAndDeniesPrivateExport() {
        DisposableKey fixture;
        Device first(fixture.authority);
        const auto spki = QByteArray::fromBase64(first.publicKey().toLatin1());
        QCOMPARE(spki.size(), 91);
        QCOMPARE(Device(fixture.authority).publicKey(), first.publicKey());
        const QByteArray message("independent process proof");
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--sign-probe", fixture.authority, QString::fromLatin1(message.toBase64())});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 0);
        const auto reply = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
        QVERIFY(verify(spki, message, QByteArray::fromBase64(reply.value("signature").toString().toLatin1())));
        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE key = 0;
        QCOMPARE(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0), SECURITY_STATUS(ERROR_SUCCESS));
        const QString name = Device::keyName(fixture.authority);
        QCOMPARE(NCryptOpenKey(provider, &key, reinterpret_cast<const wchar_t*>(name.utf16()), 0, NCRYPT_SILENT_FLAG), SECURITY_STATUS(ERROR_SUCCESS));
        DWORD policy = 1, usage = 0, actual = 0, privateSize = 0;
        const auto policyStatus = NCryptGetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), &actual, 0);
        const auto usageStatus = NCryptGetProperty(key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage), sizeof(usage), &actual, 0);
        QByteArray privateBuffer(1024, '\0');
        const auto exportStatus = NCryptExportKey(key, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr,
            reinterpret_cast<PBYTE>(privateBuffer.data()), DWORD(privateBuffer.size()), &privateSize, NCRYPT_SILENT_FLAG);
        SecureZeroMemory(privateBuffer.data(), size_t(privateBuffer.size()));
        NCryptFreeObject(key); NCryptFreeObject(provider);
        QCOMPARE(policyStatus, SECURITY_STATUS(ERROR_SUCCESS)); QCOMPARE(policy, DWORD(0));
        QCOMPARE(usageStatus, SECURITY_STATUS(ERROR_SUCCESS)); QCOMPARE(usage, DWORD(NCRYPT_ALLOW_SIGNING_FLAG));
        QVERIFY(exportStatus != ERROR_SUCCESS);
    }

    void signingNeverCreatesMissingKey() {
        DisposableKey fixture;
        const Device key(fixture.authority);
        QCOMPARE(key.sign("missing-key-proof").status, Signature::NoKey);
        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE opened = 0;
        QCOMPARE(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0), SECURITY_STATUS(ERROR_SUCCESS));
        const QString name = Device::keyName(fixture.authority);
        const auto status = NCryptOpenKey(provider, &opened, reinterpret_cast<const wchar_t*>(name.utf16()), 0, NCRYPT_SILENT_FLAG);
        if (opened) NCryptFreeObject(opened);
        NCryptFreeObject(provider);
        QVERIFY(status == NTE_BAD_KEYSET || status == NTE_NOT_FOUND);
    }

    void unusableExistingKeyIsNotReplaced() {
        DisposableKey fixture;
        NCRYPT_PROV_HANDLE provider = 0;
        NCRYPT_KEY_HANDLE key = 0;
        const QString name = Device::keyName(fixture.authority);
        QCOMPARE(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0), SECURITY_STATUS(ERROR_SUCCESS));
        QCOMPARE(NCryptCreatePersistedKey(provider, &key, NCRYPT_ECDSA_P256_ALGORITHM,
                     reinterpret_cast<const wchar_t*>(name.utf16()), 0, 0), SECURITY_STATUS(ERROR_SUCCESS));
        DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG;
        QCOMPARE(NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), 0), SECURITY_STATUS(ERROR_SUCCESS));
        QCOMPARE(NCryptFinalizeKey(key, NCRYPT_SILENT_FLAG), SECURITY_STATUS(ERROR_SUCCESS));
        NCryptFreeObject(key); NCryptFreeObject(provider);
        const Device device(fixture.authority);
        QVERIFY(device.publicKey().isEmpty());
        QCOMPARE(device.sign("existing-key-proof").status, Signature::Failed);
        const auto config = BrokerDeviceKey::clientConfig(fixture.authority.section(':', 0, 0), 443, {}, {}, {});
        try {
            config.devicePublicKey();
            QFAIL("provider failure must not silently create an unbound grant");
        } catch (const PlankBrokerError& error) {
            QCOMPARE(error.kind(), PlankBrokerError::DeviceKeyUnavailable);
        }
        DWORD actual = 0;
        QCOMPARE(NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0), SECURITY_STATUS(ERROR_SUCCESS));
        QCOMPARE(NCryptOpenKey(provider, &key, reinterpret_cast<const wchar_t*>(name.utf16()), 0, NCRYPT_SILENT_FLAG), SECURITY_STATUS(ERROR_SUCCESS));
        QCOMPARE(NCryptGetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy), sizeof(policy), &actual, 0), SECURITY_STATUS(ERROR_SUCCESS));
        NCryptFreeObject(key); NCryptFreeObject(provider);
        QCOMPARE(policy, DWORD(NCRYPT_ALLOW_EXPORT_FLAG));
    }
#endif
};

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
    if (argc == 3 && QByteArray(argv[1]) == "--resume-probe") {
        QCoreApplication app(argc, argv);
        QCoreApplication::setOrganizationName("PLANK-Device-Tests");
        QCoreApplication::setApplicationName("Disposable");
        QStandardPaths::setTestModeEnabled(true);
        BrokerSessionStore::Saved saved;
        const bool restored = BrokerSessionStore::load(QString::fromUtf8(argv[2]), saved) &&
            saved.deviceBound && !saved.legacy && saved.username == "confirmed-artist" && saved.token == "token";
        saved.token.fill(QChar('\0'));
        QFile output; output.open(stdout, QIODevice::WriteOnly);
        output.write(QJsonDocument(QJsonObject{{"restored", restored}}).toJson(QJsonDocument::Compact));
        output.flush();
        return restored ? 0 : 1;
    }
    if (argc == 4 && QByteArray(argv[1]) == "--sign-probe") {
        QCoreApplication app(argc, argv);
        const auto signature = Device(QString::fromUtf8(argv[2])).sign(QByteArray::fromBase64(argv[3]));
        QFile output; output.open(stdout, QIODevice::WriteOnly);
        output.write(QJsonDocument(QJsonObject{{"signature", signature.signature}}).toJson(QJsonDocument::Compact));
        output.flush();
        return signature.status == Signature::Signed ? 0 : 1;
    }
#endif
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    TestWindowsDeviceKey test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_windowsdevicekey.moc"

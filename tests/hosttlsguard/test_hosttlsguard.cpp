#include "../../app/backend/hosttlsguard.h"
#include <QFile>
#include <QProcess>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>

class Server : public QTcpServer
{
public:
    QList<QSslCertificate> chain;
    QSslKey key;
    QByteArray received;
    QList<QSslSocket*> sockets;
    int connections = 0;
    ~Server() override { for (auto* socket : sockets) { socket->disconnect(this); socket->abort(); } }
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        sockets.append(socket);
        ++connections;
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificateChain(chain);
        socket->setPrivateKey(key);
        socket->setProtocol(QSsl::TlsV1_3);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        connect(socket, &QSslSocket::readyRead, this, [this, socket] {
            received += socket->readAll();
            if (received.endsWith("synthetic-credential"))
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
        });
        socket->startServerEncryption();
    }
};

class HostTlsGuardTest : public QObject
{
    Q_OBJECT
    QTemporaryDir files;
    QNetworkReply::NetworkError lastCompletionError = QNetworkReply::NoError;
    bool crypto(QStringList args)
    {
        QProcess process;
        process.setWorkingDirectory(files.path());
#ifdef Q_OS_MACOS
        process.start("/usr/bin/openssl", args);
#else
        process.start("openssl", args);
#endif
        const bool ok = process.waitForFinished(20000) && process.exitCode() == 0;
        if (!ok) qWarning() << "Synthetic TLS fixture failed:" << process.errorString() << process.readAllStandardError();
        return ok;
    }
    QByteArray read(const QString& name)
    {
        QFile file(files.filePath(name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QSslCertificate cert(const QString& name) { return QSslCertificate(read(name + ".pem")); }
    QSslKey key(const QString& name) { return QSslKey(read(name + ".key"), QSsl::Rsa); }
    bool request(QNetworkAccessManager& manager, Server& server, const HostTrustStore& store,
                 HostTlsGuard::Mode mode, const QByteArray& expected = {})
    {
        QUrl url(QString("https://127.0.0.1:%1/login").arg(server.serverPort()));
        HostTlsGuard guard(manager, store, HostTrustStore::endpoint(url), mode, expected);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto config = QSslConfiguration::defaultConfiguration();
        config.setProtocol(QSsl::TlsV1_3);
        request.setSslConfiguration(config);
        std::unique_ptr<QNetworkReply> reply(manager.post(request, "synthetic-credential"));
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(5000);
        if (!reply->isFinished()) loop.exec();
        lastCompletionError = guard.completionError(reply->error());
        return guard.checked() && reply->error() == QNetworkReply::NoError && reply->readAll() == "ok";
    }
private slots:
    void failuresBeforeTlsRemainTransportErrors_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("worker-not-listening") << 0 << int(QNetworkReply::ConnectionRefusedError);
        QTest::newRow("worker-closes-during-startup") << 1 << int(QNetworkReply::RemoteHostClosedError);
        QTest::newRow("worker-handshake-times-out") << 2 << int(QNetworkReply::OperationCanceledError);
    }
    void failuresBeforeTlsRemainTransportErrors()
    {
        QFETCH(int, kind);
        QFETCH(int, expected);
        QTemporaryDir state;
        HostTrustStore store(state.filePath("trust"));
        QNetworkAccessManager manager;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto port = server.serverPort();
        QByteArray received;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, &server, [&, socket] { received += socket->readAll(); });
            if (kind == 1) socket->disconnectFromHost();
        });
        if (kind == 0) server.close();
        const QUrl url(QString("https://127.0.0.1:%1/login").arg(port));
        HostTlsGuard guard(manager, store, HostTrustStore::endpoint(url), HostTlsGuard::Mode::RequireKnown);
        std::unique_ptr<QNetworkReply> reply(manager.post(QNetworkRequest(url), "synthetic-credential"));
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, reply.get(), &QNetworkReply::abort);
        connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(kind == 2 ? 200 : 3000);
        if (!reply->isFinished()) loop.exec();
        QVERIFY(!guard.checked());
        if (kind == 0) {
            // Qt's macOS/OpenSSL backend may report an immediate refusal as
            // UnknownNetworkError. Preserve either transport classification.
            QVERIFY(reply->error() == QNetworkReply::ConnectionRefusedError ||
                    reply->error() == QNetworkReply::UnknownNetworkError);
        } else {
            QCOMPARE(int(reply->error()), expected);
        }
        QCOMPARE(guard.completionError(reply->error()), reply->error());
        QVERIFY(!received.contains("synthetic-credential"));
    }
    void successfulReplyRequiresValidation()
    {
        QTemporaryDir state;
        HostTrustStore store(state.filePath("trust"));
        QNetworkAccessManager manager;
        HostTlsGuard guard(manager, store, "https://plank-host:28989", HostTlsGuard::Mode::Observe);
        QCOMPARE(guard.completionError(QNetworkReply::NoError), QNetworkReply::SslHandshakeFailedError);
    }
    void initTestCase()
    {
        // Match app/main.cpp: Apple's legacy SecureTransport backend cannot
        // exercise PLANK's TLS 1.3 contract. Never lower the test protocol.
        QVERIFY(QSslSocket::setActiveBackend(QStringLiteral("openssl")));
        QVERIFY(QSslSocket::supportsSsl());
        QVERIFY(files.isValid());
        for (const QString& name : {QString("machine"), QString("stranger")}) {
            QVERIFY(crypto({"req", "-x509", "-newkey", "rsa:3072", "-nodes", "-days", "2", "-sha256",
                "-subj", "/CN=PLANK test", "-addext", "subjectAltName=DNS:plank-host",
                "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
                "-addext", "keyUsage=critical,digitalSignature,keyCertSign",
                "-keyout", name + ".key", "-out", name + ".pem"}));
        }
        QVERIFY(crypto({"req", "-new", "-x509", "-key", "machine.key", "-days", "3", "-sha256",
            "-subj", "/CN=Renewed", "-addext", "subjectAltName=DNS:plank-host", "-out", "renewed.pem"}));
        QVERIFY(crypto({"req", "-new", "-newkey", "rsa:3072", "-nodes", "-subj", "/CN=worker",
            "-keyout", "worker.key", "-out", "worker.csr"}));
        QFile extensions(files.filePath("extensions"));
        QVERIFY(extensions.open(QIODevice::WriteOnly));
        extensions.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:plank-host\n");
        extensions.close();
        QVERIFY(crypto({"x509", "-req", "-in", "worker.csr", "-CA", "machine.pem", "-CAkey", "machine.key",
            "-set_serial", "123", "-days", "1", "-extfile", "extensions", "-out", "worker.pem"}));
    }
    void certificatesRenewWithoutChangingIdentity()
    {
        const auto identity = HostTlsGuard::identityKey({cert("machine")});
        QCOMPARE(identity.size(), 32);
        QCOMPARE(HostTlsGuard::identityKey({cert("renewed")}), identity);
        QCOMPARE(HostTlsGuard::identityKey({cert("worker"), cert("machine")}), identity);
        QVERIFY(HostTlsGuard::identityKey({cert("stranger")}) != identity);
        QVERIFY(HostTlsGuard::identityKey({cert("worker"), cert("stranger")}).isEmpty());
        QVERIFY(HostTlsGuard::identityKey({cert("worker")}).isEmpty());
        QVERIFY(HostTlsGuard::identityKey({cert("machine"), cert("machine")}).isEmpty());
    }
    void rejectedHostReceivesNoHttpBytes()
    {
        QTemporaryDir state;
        HostTrustStore store(state.filePath("trust"));
        QNetworkAccessManager manager;
        Server server;
        server.chain = {cert("machine")}; server.key = key("machine");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto endpoint = HostTrustStore::endpoint(QUrl(QString("https://127.0.0.1:%1").arg(server.serverPort())));
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::RequireKnown));
        QCOMPARE(lastCompletionError, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(server.received, QByteArray());
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QVERIFY(server.received.contains("synthetic-credential"));
        const auto identity = HostTlsGuard::identityKey({cert("machine")});
        QCOMPARE(store.check(endpoint, identity).status, HostTrustStore::Status::Trusted);
        // Same authority, different worker key: no re-enrollment or exception.
        server.received.clear();
        server.chain = {cert("worker"), cert("machine")}; server.key = key("worker");
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::RequireKnown, identity));
        QCOMPARE(server.connections, 3); // keep-alive never skips the TLS gate
        server.received.clear();
        server.chain = {cert("stranger")}; server.key = key("stranger");
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::RequireKnown));
        QCOMPARE(lastCompletionError, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(server.received, QByteArray());
        // Consent to a replacement cannot send an old conversation's password.
        auto replacement = HostTlsGuard::identityKey({cert("stranger")});
        QCOMPARE(store.replace(endpoint, identity, replacement).status, HostTrustStore::Status::Trusted);
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::RequireKnown, identity));
        QCOMPARE(lastCompletionError, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(server.received, QByteArray());
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::RequireKnown, replacement));
    }
    void corruptStoreAndForgedChainSendNothing()
    {
        QTemporaryDir state;
        HostTrustStore store(state.filePath("trust"));
        QNetworkAccessManager manager;
        Server server;
        server.chain = {cert("worker"), cert("stranger")}; server.key = key("worker");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QCOMPARE(lastCompletionError, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(server.received, QByteArray());
        server.chain = {cert("machine")}; server.key = key("machine");
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QFile damaged(state.filePath("trust/identities.json"));
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        damaged.write("broken"); damaged.close();
        server.received.clear();
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QCOMPARE(lastCompletionError, QNetworkReply::SslHandshakeFailedError);
        QCOMPARE(server.received, QByteArray());
    }
    void observeValidatedTlsWithoutEnrolling()
    {
        QTemporaryDir state;
        HostTrustStore store(state.filePath("trust"));
        QNetworkAccessManager manager;
        Server server;
        server.chain = {cert("machine")}; server.key = key("machine");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::Observe));
        QCOMPARE(lastCompletionError, QNetworkReply::NoError);
        QVERIFY(!QFile::exists(state.filePath("trust/identities.json")));
    }
};
QTEST_GUILESS_MAIN(HostTlsGuardTest)
#include "test_hosttlsguard.moc"

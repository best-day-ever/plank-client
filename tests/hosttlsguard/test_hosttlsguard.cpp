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
        return guard.checked() && reply->error() == QNetworkReply::NoError && reply->readAll() == "ok";
    }
private slots:
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
        QCOMPARE(server.received, QByteArray());
        // Consent to a replacement cannot send an old conversation's password.
        auto replacement = HostTlsGuard::identityKey({cert("stranger")});
        QCOMPARE(store.replace(endpoint, identity, replacement).status, HostTrustStore::Status::Trusted);
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::RequireKnown, identity));
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
        QCOMPARE(server.received, QByteArray());
        server.chain = {cert("machine")}; server.key = key("machine");
        QVERIFY(request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QFile damaged(state.filePath("trust/identities.json"));
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        damaged.write("broken"); damaged.close();
        server.received.clear();
        QVERIFY(!request(manager, server, store, HostTlsGuard::Mode::Enroll));
        QCOMPARE(server.received, QByteArray());
    }
};
QTEST_GUILESS_MAIN(HostTlsGuardTest)
#include "test_hosttlsguard.moc"

#include <QtTest>

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "relaywakeclient.h"

class TestRelayWakeClient : public QObject
{
    Q_OBJECT

private slots:
    void successfulEmptyRequest();
    void relayErrorIsReported();
};

void TestRelayWakeClient::successfulEmptyRequest()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    bool receivedPayload = false;
    connect(&server, &QTcpServer::newConnection, this, [&]() {
        QTcpSocket* socket = server.nextPendingConnection();
        QVERIFY(socket != nullptr);
        QTimer::singleShot(20, socket, [socket, &receivedPayload]() {
            receivedPayload = socket->bytesAvailable() != 0;
            socket->write("OK wake request accepted\n");
            socket->disconnectFromHost();
        });
    });

    auto* request = new RelayWakeClient(
                QStringLiteral("127.0.0.1"), server.serverPort(), this);
    QSignalSpy completion(request, &RelayWakeClient::completed);
    request->start();

    QVERIFY(completion.wait(1000));
    QCOMPARE(completion.count(), 1);
    QVERIFY(!completion.at(0).at(0).isValid());
    QVERIFY(!receivedPayload);
}

void TestRelayWakeClient::relayErrorIsReported()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, this, [&]() {
        QTcpSocket* socket = server.nextPendingConnection();
        QVERIFY(socket != nullptr);
        socket->write("ERROR wake MAC is not configured for this forwarding row\n");
        socket->disconnectFromHost();
    });

    auto* request = new RelayWakeClient(
                QStringLiteral("127.0.0.1"), server.serverPort(), this);
    QSignalSpy completion(request, &RelayWakeClient::completed);
    request->start();

    QVERIFY(completion.wait(1000));
    QCOMPARE(completion.count(), 1);
    QCOMPARE(
        completion.at(0).at(0).toString(),
        QStringLiteral("wake MAC is not configured for this forwarding row"));
}

QTEST_GUILESS_MAIN(TestRelayWakeClient)
#include "test_relaywakeclient.moc"

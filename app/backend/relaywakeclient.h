#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariant>

class QTcpSocket;
class QTimer;

class RelayWakeClient : public QObject
{
    Q_OBJECT

public:
    explicit RelayWakeClient(QString address, quint16 port,
                             QObject* parent = nullptr);

    void start();

signals:
    void completed(QVariant error);

private:
    void handleReadyRead();
    void finish(const QString& error = QString());

    QString m_Address;
    quint16 m_Port;
    QTcpSocket* m_Socket;
    QTimer* m_Timeout;
    QByteArray m_Response;
    bool m_Complete;
};

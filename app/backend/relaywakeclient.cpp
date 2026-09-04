#include "relaywakeclient.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace {
constexpr int WakeRequestTimeoutMs = 5000;
constexpr int MaximumResponseBytes = 512;
}

RelayWakeClient::RelayWakeClient(QString address, quint16 port, QObject* parent)
    : QObject(parent),
      m_Address(std::move(address)),
      m_Port(port),
      m_Socket(new QTcpSocket(this)),
      m_Timeout(new QTimer(this)),
      m_Complete(false)
{
    m_Timeout->setSingleShot(true);
    connect(m_Timeout, &QTimer::timeout, this, [this]() {
        finish(tr("The PLANK Relay wake service did not respond."));
    });
    connect(m_Socket, &QTcpSocket::readyRead,
            this, &RelayWakeClient::handleReadyRead);
    connect(m_Socket, &QTcpSocket::disconnected, this, [this]() {
        handleReadyRead();
        if (!m_Complete) {
            finish(tr("The PLANK Relay wake service closed the connection without a response."));
        }
    });
    connect(m_Socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
        if (!m_Complete) {
            finish(tr("Unable to reach the PLANK Relay wake service: %1")
                   .arg(m_Socket->errorString()));
        }
    });
}

void RelayWakeClient::start()
{
    m_Timeout->start(WakeRequestTimeoutMs);
    m_Socket->connectToHost(m_Address, m_Port);
}

void RelayWakeClient::handleReadyRead()
{
    if (m_Complete) {
        return;
    }

    m_Response.append(m_Socket->readAll());
    if (m_Response.size() > MaximumResponseBytes) {
        finish(tr("The PLANK Relay wake service returned an invalid response."));
        return;
    }

    const int newline = m_Response.indexOf('\n');
    if (newline < 0) {
        return;
    }

    const QString response = QString::fromUtf8(
                m_Response.constData(), newline).trimmed();
    if (response.startsWith(QStringLiteral("OK "))) {
        finish();
    }
    else if (response.startsWith(QStringLiteral("ERROR "))) {
        finish(response.mid(6));
    }
    else {
        finish(tr("The PLANK Relay wake service returned an invalid response."));
    }
}

void RelayWakeClient::finish(const QString& error)
{
    if (m_Complete) {
        return;
    }
    m_Complete = true;
    m_Timeout->stop();
    m_Socket->abort();
    emit completed(error.isEmpty() ? QVariant() : QVariant(error));
    deleteLater();
}

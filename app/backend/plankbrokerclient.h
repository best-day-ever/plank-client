#pragma once

#include "plankbroker.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <exception>

// Blocking HTTPS client for the PLANK broker public API (bde-linux
// docs/plank-broker.md section 10.1). Each request uses a fresh
// QNetworkAccessManager and a local event loop, so calls are safe from any
// thread (GUI workers and Session reconnect threads alike) and a TLS session
// is never reused across a trust decision.
//
// Trust: TLS 1.3 only; the broker leaf is accepted if and only if its SPKI
// SHA-256 matches one configured pin. WebPKI, host name and expiry are not
// consulted. An empty pin list refuses to connect.
class PlankBrokerError : public std::exception
{
public:
    enum Kind {
        NotConfigured,   // no valid pin / host
        Network,         // unreachable, timeout
        Tls,             // pin mismatch, not TLS 1.3
        Denied,          // generic auth failure / host not admitted
        RateLimited,     // HTTP 429, see retryAfter()
        SessionExpired,  // bearer rejected; sign in again
        Protocol,        // malformed reply
    };

    PlankBrokerError(Kind kind, int retryAfter = 0) : m_Kind(kind), m_RetryAfter(retryAfter) {}

    Kind kind() const { return m_Kind; }
    int retryAfter() const { return m_RetryAfter; }
    const char* what() const noexcept override { return "PLANK broker request failed"; }

    // Deliberately generic, user-facing text; no broker-supplied strings.
    QString userMessage() const;

private:
    Kind m_Kind;
    int m_RetryAfter;
};

class PlankBrokerClient
{
public:
    struct Config {
        QString host;
        quint16 port = PlankBroker::DefaultPort;
        QStringList pins;
    };

    explicit PlankBrokerClient(Config config);

    const Config& config() const { return m_Config; }

    // Throws PlankBrokerError::NotConfigured unless host and >=1 valid pin.
    void checkConfigured() const;

    // Sign-in conversation. Challenge/Authenticated are returned; Denied,
    // RateLimited and malformed replies throw.
    PlankBroker::AuthReply start(const QString& username) const;
    PlankBroker::AuthReply respond(const QString& conversationId, const QJsonArray& responses) const;

    QVector<PlankBroker::Host> hosts(const QString& sessionToken) const;
    PlankBroker::Lease connect(const QString& sessionToken, const QString& hostId) const;
    void keepalive(const QString& sessionToken, const QString& hostId) const;
    void logout(const QString& sessionToken) const;

private:
    struct Response {
        int status = 0;
        QByteArray body;
    };

    Response request(const QByteArray& method, const QString& path,
                     const QJsonObject* body, const QString& sessionToken) const;
    static void throwForBearerStatus(int status, const QByteArray& body);

    Config m_Config;
};

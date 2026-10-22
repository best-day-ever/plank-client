#pragma once

#include "plankbroker.h"

#include <QJsonObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <exception>
#include <functional>

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
        Unavailable,     // host temporarily offline during desktop handoff
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
    // Device-bound sessions (section 14.2). Both are optional and called on
    // the requesting thread; on macOS they run the plank-passkey helper.
    // devicePublicKey: the base64 SPKI of this device's key for the broker
    //   (created on first use), or empty to sign in unbound.
    // deviceSigner: a signature over a proof message. NoKey (this device has
    //   no key for the broker, so the session is unbound) sends the request
    //   without proof headers; Failed (Mac locked, helper error) sends nothing
    //   and throws a retryable Network error, so a bound session is never
    //   signed out just because signing was briefly impossible.
    struct DeviceSignature {
        enum Status { Signed, NoKey, Failed };
        Status status = Failed;
        QString signature;
    };
    using DevicePublicKeyProvider = std::function<QString()>;
    using DeviceSigner = std::function<DeviceSignature(const QByteArray& message)>;

    struct Config {
        QString host;
        quint16 port = PlankBroker::DefaultPort;
        QStringList pins;
        DevicePublicKeyProvider devicePublicKey;
        DeviceSigner deviceSigner;
    };

    explicit PlankBrokerClient(Config config);

    const Config& config() const { return m_Config; }

    // Throws PlankBrokerError::NotConfigured unless host and >=1 valid pin.
    void checkConfigured() const;

    // Sign-in conversation. Challenge/Authenticated are returned; Denied,
    // RateLimited and malformed replies throw. PasswordOtp sends exactly
    // {"username"}; Passkey adds "method":"passkey" (section 13.3). Either
    // adds "device_key" when config().devicePublicKey yields one (14.2).
    PlankBroker::AuthReply start(const QString& username,
                                 PlankBroker::AuthMethod method = PlankBroker::AuthMethod::PasswordOtp) const;
    PlankBroker::AuthReply respond(const QString& conversationId, const QJsonArray& responses) const;
    PlankBroker::AuthReply respondPasskey(const QString& conversationId,
                                          const PlankBroker::PasskeyAssertion& assertion) const;

    // Passkey sign-in (section 13.3/13.4): start with method "passkey", let
    // `assertor` sign the challenge (the plank-passkey helper; Touch ID), then
    // respond. Anything that means "this Mac cannot sign in with a passkey for
    // this user" - no matching local key, a broker denial, a broker that
    // offers no passkey prompt, a helper failure - yields Fallback, and the
    // caller asks for password + code instead. Network, TLS, rate limiting and
    // malformed replies throw as for start()/respond().
    enum class PasskeyAssertResult { Signed, NoMatchingKey, NotConfirmed, Failed };
    using PasskeyAssertor = std::function<PasskeyAssertResult(const PlankBroker::PasskeyRequest& request,
                                                              PlankBroker::PasskeyAssertion& assertion)>;
    struct PasskeySignIn {
        enum Result { Authenticated, Fallback, NotConfirmed };
        Result result = Fallback;
        PlankBroker::AuthReply reply;
    };
    PasskeySignIn signInWithPasskey(const QString& username, const QString& rpId,
                                    const PasskeyAssertor& assertor) const;

    // Unauthenticated JSON POST with the same TLS 1.3 + pin rules as every
    // call (used by the enrolment conversation, section 16). Returns the HTTP
    // status and body; the caller parses and zeroes the body.
    struct Response {
        int status = 0;
        QByteArray body;
    };
    Response post(const QString& path, const QJsonObject& body) const;
    Response get(const QString& path) const;
    // Streams a release into a new file. The same broker TLS pin is checked
    // before any bytes are accepted; size and SHA-256 are checked before use.
    void download(const QString& path, const QString& destination,
                  qint64 expectedSize, const QByteArray& expectedSha256) const;

    // Bearer calls. With a deviceSigner each carries X-Plank-Device-Time and
    // X-Plank-Device-Proof (section 14.2) when the signer produces a proof.
    QVector<PlankBroker::Host> hosts(const QString& sessionToken) const;
    PlankBroker::Lease connect(const QString& sessionToken, const QString& hostId) const;
    void keepalive(const QString& sessionToken, const QString& hostId) const;
    void logout(const QString& sessionToken) const;
    // A fresh password + OTP session may register one key with the user's
    // own IPA rights. The broker holds the proven password only briefly;
    // skipping the offer discards it immediately.
    void setupPasskey(const QString& sessionToken, const QString& mapping) const;
    void skipPasskeySetup(const QString& sessionToken) const;

private:
    Response request(const QByteArray& method, const QString& path,
                     const QJsonObject* body, const QString& sessionToken) const;
    static void throwForBearerStatus(int status, const QByteArray& body);
    PlankBroker::AuthReply sendRespond(const QJsonObject& body) const;

    Config m_Config;
};

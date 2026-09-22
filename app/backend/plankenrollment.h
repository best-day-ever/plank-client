#pragma once

// First sign-in through the broker: prove the one-time password an
// administrator issued, choose a new password when it has expired, add an
// authenticator app (TOTP) and optionally Touch ID, and end signed in like a
// normal password + code login.
//
// Contract: bde-linux docs/plank-broker.md section 16 (the /v1/enroll/*
// endpoints; same TLS, pinning and JSON conventions as section 10.1). Every
// outcome is an enumerated state; the Client never shows broker text, and
// any state it does not know is treated as "denied".
//
// The reply parsing and password checks are pure (tests/plankbroker). The
// Conversation below drives the endpoints over PlankBrokerClient; it blocks,
// so it runs on a worker thread.

#include "plankbroker.h"
#include "plankbrokerclient.h"

#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QUrlQuery>

namespace PlankEnrollment
{

enum class Endpoint { Start, Respond, Password, Totp, Passkey, Finish };

inline QString path(Endpoint endpoint)
{
    switch (endpoint) {
    case Endpoint::Start: return QStringLiteral("/v1/enroll/start");
    case Endpoint::Respond: return QStringLiteral("/v1/enroll/respond");
    case Endpoint::Password: return QStringLiteral("/v1/enroll/password");
    case Endpoint::Totp: return QStringLiteral("/v1/enroll/totp");
    case Endpoint::Passkey: return QStringLiteral("/v1/enroll/passkey");
    case Endpoint::Finish: default: return QStringLiteral("/v1/enroll/finish");
    }
}

enum class State {
    Malformed,
    RateLimited,
    Challenge,         // start: conversation_id + prompts
    Denied,
    AlreadyEnrolled,
    NewPassword,
    Totp,
    PasswordRejected,
    Authenticated,
    Enrolled,
    CodeRejected,
    PasskeyAdded,
    PasskeyRejected,
    Done,
};

enum class RejectReason { TooShort, TooSimple, Reused, Policy };

struct PasswordPolicy {
    int minLength = 0;
    int minClasses = 0;
};

struct Reply {
    State state = State::Malformed;
    int retryAfter = 0;
    PlankBroker::AuthReply challenge;   // Start: conversation id and prompts
    PasswordPolicy policy;              // NewPassword
    QString otpauthUri;                 // Totp
    QString secret;                     // Totp (base32)
    RejectReason reason = RejectReason::Policy; // PasswordRejected
    bool passkeyAvailable = false;      // Authenticated, Enrolled
    PlankBroker::AuthReply session;     // Authenticated: exactly /v1/auth/respond's fields
};

constexpr int MaximumOtpauthUriLength = 1024;
constexpr int MinimumSecretLength = 16;   // 80 bits of base32
constexpr int MaximumSecretLength = 128;
constexpr int MaximumMappingLength = 2048;
constexpr int MaximumPasswordLength = 256;

// Upper-case RFC 4648 base32 without padding, as authenticator apps expect.
inline bool isBase32Secret(const QString& secret)
{
    if (secret.size() < MinimumSecretLength || secret.size() > MaximumSecretLength) return false;
    for (const QChar c : secret) {
        const ushort u = c.unicode();
        if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7'))) return false;
    }
    return true;
}

// otpauth://totp/<label>?secret=<secret>&... whose secret is the one shown
// as text, so the QR code and the typed key always create the same token.
inline bool isOtpauthUri(const QString& uri, const QString& secret)
{
    if (!PlankBroker::isPrintableAscii(uri, MaximumOtpauthUriLength) ||
            !uri.startsWith(QLatin1String("otpauth://totp/"))) {
        return false;
    }
    const QUrl url(uri, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("otpauth") || url.host() != QLatin1String("totp")) {
        return false;
    }
    const QUrlQuery query(url);
    const QStringList secrets = query.allQueryItemValues(QStringLiteral("secret"));
    return secrets.size() == 1 && secrets.first().toUpper() == secret;
}

// "ABCD EFGH IJKL ..." for reading aloud or typing into an app.
inline QString groupSecret(const QString& secret)
{
    QString grouped;
    for (int i = 0; i < secret.size(); ++i) {
        if (i > 0 && i % 4 == 0) grouped.append(QLatin1Char(' '));
        grouped.append(secret.at(i));
    }
    return grouped;
}

// The plank-passkey helper's "passkey:<credential id>,<SPKI>" (both standard
// base64), which IPA stores as the user's ipapasskey value.
inline bool isPasskeyMapping(const QString& mapping)
{
    if (mapping.size() > MaximumMappingLength || !mapping.startsWith(QLatin1String("passkey:"))) return false;
    const QStringList parts = mapping.mid(8).split(QLatin1Char(','));
    QByteArray decoded;
    return parts.size() == 2 && PlankBroker::decodeStandardBase64(parts.at(0), decoded) &&
            PlankBroker::decodeStandardBase64(parts.at(1), decoded);
}

// Character classes as FreeIPA counts them: lower case, upper case, digits,
// other ASCII, and characters beyond ASCII.
inline int characterClasses(const QString& password)
{
    bool lower = false, upper = false, digit = false, other = false, wide = false;
    for (const QChar c : password) {
        const ushort u = c.unicode();
        if (u >= 'a' && u <= 'z') lower = true;
        else if (u >= 'A' && u <= 'Z') upper = true;
        else if (u >= '0' && u <= '9') digit = true;
        else if (u < 0x80) other = true;
        else wide = true;
    }
    return int(lower) + int(upper) + int(digit) + int(other) + int(wide);
}

enum class PasswordCheck { Ok, Empty, Mismatch, TooShort, TooSimple, TooLong };

// Local checks before a new password is sent; the broker (IPA) still decides.
inline PasswordCheck checkNewPassword(const QString& password, const QString& confirmation,
                                      const PasswordPolicy& policy)
{
    if (password.isEmpty()) return PasswordCheck::Empty;
    if (password != confirmation) return PasswordCheck::Mismatch;
    if (password.size() > MaximumPasswordLength) return PasswordCheck::TooLong;
    if (password.size() < policy.minLength) return PasswordCheck::TooShort;
    if (characterClasses(password) < policy.minClasses) return PasswordCheck::TooSimple;
    return PasswordCheck::Ok;
}

inline bool parsePolicy(const QJsonValue& value, PasswordPolicy& policy)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QJsonValue length = object.value(QStringLiteral("min_length"));
    const QJsonValue classes = object.value(QStringLiteral("min_classes"));
    if (!length.isDouble() || !classes.isDouble() ||
            length.toDouble() != static_cast<double>(length.toInt()) ||
            classes.toDouble() != static_cast<double>(classes.toInt()) ||
            length.toInt() < 0 || length.toInt() > MaximumPasswordLength ||
            classes.toInt() < 0 || classes.toInt() > 5) {
        return false;
    }
    policy.minLength = length.toInt();
    policy.minClasses = classes.toInt();
    return true;
}

// Maps HTTP status + body of one enrolment endpoint. 429 is RateLimited,
// anything not HTTP 200 JSON is Malformed. A state the endpoint may not
// answer with, or an unknown one, is Denied; a known state with invalid
// fields is Malformed (the Client stops rather than guessing).
inline Reply parseReply(Endpoint endpoint, int httpStatus, const QByteArray& body)
{
    Reply reply;
    QJsonObject object;
    const bool parsed = PlankBroker::parseObject(body, object);
    if (httpStatus == 429) {
        reply.state = State::RateLimited;
        reply.retryAfter = parsed ? PlankBroker::retryAfterFrom(object) : PlankBroker::DefaultRetryAfterSeconds;
        return reply;
    }
    if (httpStatus != 200 || !parsed) return reply;

    const QString state = object.value(QStringLiteral("state")).toString();
    if (endpoint == Endpoint::Start) {
        // The start reply carries no state; "denied" is tolerated as such.
        if (state == QLatin1String("denied")) {
            reply.state = State::Denied;
        } else if ((state.isEmpty() || state == QLatin1String("challenge")) &&
                   PlankBroker::parseChallenge(object, reply.challenge)) {
            reply.state = State::Challenge;
        }
        return reply;
    }
    if (endpoint == Endpoint::Finish) {
        reply.state = state == QLatin1String("done") ? State::Done : State::Denied;
        return reply;
    }

    auto allowed = [endpoint](State candidate) {
        switch (endpoint) {
        case Endpoint::Respond:
            return candidate == State::AlreadyEnrolled || candidate == State::NewPassword ||
                    candidate == State::Totp;
        case Endpoint::Password:
            return candidate == State::Totp || candidate == State::PasswordRejected;
        case Endpoint::Totp:
            return candidate == State::Authenticated || candidate == State::Enrolled ||
                    candidate == State::CodeRejected;
        case Endpoint::Passkey:
            return candidate == State::PasskeyAdded || candidate == State::PasskeyRejected;
        default:
            return false;
        }
    };
    static const QList<QPair<QString, State>> names = {
        {QStringLiteral("already_enrolled"), State::AlreadyEnrolled},
        {QStringLiteral("new_password"), State::NewPassword},
        {QStringLiteral("totp"), State::Totp},
        {QStringLiteral("password_rejected"), State::PasswordRejected},
        {QStringLiteral("authenticated"), State::Authenticated},
        {QStringLiteral("enrolled"), State::Enrolled},
        {QStringLiteral("code_rejected"), State::CodeRejected},
        {QStringLiteral("passkey_added"), State::PasskeyAdded},
        {QStringLiteral("passkey_rejected"), State::PasskeyRejected},
    };
    State candidate = State::Denied;
    for (const auto& name : names) {
        if (name.first == state) candidate = name.second;
    }
    if (!allowed(candidate)) {
        reply.state = State::Denied;
        return reply;
    }

    const QJsonValue passkey = object.value(QStringLiteral("passkey_available"));
    switch (candidate) {
    case State::NewPassword:
        if (!parsePolicy(object.value(QStringLiteral("policy")), reply.policy)) return reply;
        break;
    case State::Totp:
        reply.secret = object.value(QStringLiteral("secret")).toString();
        reply.otpauthUri = object.value(QStringLiteral("otpauth_uri")).toString();
        if (!isBase32Secret(reply.secret) || !isOtpauthUri(reply.otpauthUri, reply.secret)) {
            reply.secret.fill(QChar('\0'));
            reply.secret.clear();
            reply.otpauthUri.fill(QChar('\0'));
            reply.otpauthUri.clear();
            return reply;
        }
        break;
    case State::PasswordRejected: {
        const QString reason = object.value(QStringLiteral("reason")).toString();
        reply.reason = reason == QLatin1String("too_short") ? RejectReason::TooShort :
                       reason == QLatin1String("too_simple") ? RejectReason::TooSimple :
                       reason == QLatin1String("reused") ? RejectReason::Reused : RejectReason::Policy;
        break;
    }
    case State::Authenticated:
        if (!PlankBroker::parseAuthenticated(object, reply.session) ||
                !(passkey.isBool() || passkey.isUndefined())) {
            reply.session = PlankBroker::AuthReply();
            return reply;
        }
        reply.passkeyAvailable = passkey.toBool(false);
        break;
    case State::Enrolled:
        if (!(passkey.isBool() || passkey.isUndefined())) return reply;
        reply.passkeyAvailable = passkey.toBool(false);
        break;
    default:
        break;
    }
    reply.state = candidate;
    return reply;
}

// ---------------------------------------------------------------------------
// Wizard flow
// ---------------------------------------------------------------------------

enum class Step {
    Welcome,
    Credentials,     // username + one-time password
    NewPassword,
    Authenticator,   // QR code / key + first code
    NextCode,        // enrolled, but the follow-up login needs the next code
    Passkey,         // optional Touch ID
    Done,
};

enum class Notice {
    None,
    Denied,                     // generic: wrong user/password, gate, expired conversation
    DeniedAfterPasswordChange,  // the new password is set; start again with it
    AlreadyEnrolled,
    PasswordTooShort,
    PasswordTooSimple,
    PasswordReused,
    PasswordPolicy,
    CodeRejected,
    NextCodeRejected,
    PasskeyNotAdded,            // the broker did not store the Touch ID key
    PasskeyNotCreated,          // this Mac could not create one
};

struct Outcome {
    Step step = Step::Credentials;
    Notice notice = Notice::None;
    PasswordPolicy policy;
    QString otpauthUri;
    QString secret;
    bool passkeyAvailable = false;
    // Set once, when the flow has produced a broker session.
    QString sessionToken;
    QString username;
    bool deviceBound = false;

    bool signedIn() const { return !sessionToken.isEmpty(); }
};

// Client-side, deliberately generic text for a notice or a local password
// check (empty for None/Ok). Never broker-supplied; no hint whether a user
// name exists.
QString noticeText(Notice notice, const PasswordPolicy& policy);
QString passwordCheckText(PasswordCheck check, const PasswordPolicy& policy);

// One enrolment conversation. Methods block (worker thread) and are
// serialised internally. Network, TLS, rate limiting, a missing
// configuration and malformed replies throw PlankBrokerError and leave the
// step unchanged, so the same screen can simply be retried. Passwords, the
// TOTP secret and the conversation id are zeroed when no longer needed.
class Conversation
{
public:
    explicit Conversation(PlankBrokerClient::Config config);
    ~Conversation();
    Conversation(const Conversation&) = delete;
    Conversation& operator=(const Conversation&) = delete;

    // POST /v1/enroll/start + /v1/enroll/respond with the one-time password.
    Outcome begin(const QString& username, QString oneTimePassword);
    // POST /v1/enroll/password.
    Outcome changePassword(QString newPassword);
    // POST /v1/enroll/totp with the first code.
    Outcome verifyCode(QString code);
    // After "enrolled": a normal /v1/auth sign-in with the kept password and
    // the authenticator's next code.
    Outcome signInWithNextCode(QString code);
    // POST /v1/enroll/passkey (after sign-in, when passkey_available).
    Outcome addPasskey(const QString& mapping);
    // POST /v1/enroll/finish, best effort; also forgets every secret.
    void finish() noexcept;

    Step step() const;
    QString username() const;
    bool hasConversation() const;

private:
    Reply send(Endpoint endpoint, QJsonObject body) const;
    Outcome deniedOutcome();
    Outcome authenticatorOutcome(Notice notice = Notice::None) const;
    void forgetSecrets();
    void finishLocked() noexcept;

    const PlankBrokerClient m_Client;
    mutable QMutex m_Lock;
    Step m_Step = Step::Credentials;
    QString m_ConversationId;
    QString m_Username;
    QString m_Password;     // the current one: one-time, then the new one
    bool m_PasswordChanged = false;
    PasswordPolicy m_Policy;
    QString m_OtpauthUri;
    QString m_Secret;
    bool m_PasskeyAvailable = false;
};

}

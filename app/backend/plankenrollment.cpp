#include "plankenrollment.h"

#include <QCoreApplication>
#include <QJsonArray>

namespace PlankEnrollment
{

namespace
{
void zero(QString& value)
{
    value.fill(QChar('\0'));
    value.clear();
}

Notice rejectionNotice(RejectReason reason)
{
    switch (reason) {
    case RejectReason::TooShort: return Notice::PasswordTooShort;
    case RejectReason::TooSimple: return Notice::PasswordTooSimple;
    case RejectReason::Reused: return Notice::PasswordReused;
    case RejectReason::Policy: default: return Notice::PasswordPolicy;
    }
}

// Throws for everything that is not an answer of the conversation itself.
void throwForTransport(const Reply& reply)
{
    if (reply.state == State::RateLimited) {
        throw PlankBrokerError(PlankBrokerError::RateLimited, reply.retryAfter);
    }
    if (reply.state == State::Malformed) {
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
}
}

QString noticeText(Notice notice, const PasswordPolicy& policy)
{
    switch (notice) {
    case Notice::None:
        return QString();
    case Notice::Denied:
        return QCoreApplication::translate("PlankEnrollment",
            "We couldn't start setup. Check your username and one-time password. "
            "If it's more than 7 days old, ask the studio for a new one.");
    case Notice::DeniedAfterPasswordChange:
        return QCoreApplication::translate("PlankEnrollment",
            "Setup was interrupted, but your new password is saved. "
            "Start again with your username and the new password.");
    case Notice::DeniedPasswordMaybeChanged:
        return QCoreApplication::translate("PlankEnrollment",
            "Setup was interrupted, and your new password may already be saved. "
            "Start again with your username and the new password. "
            "If that doesn't work, use your one-time password.");
    case Notice::AlreadyEnrolled:
        return QCoreApplication::translate("PlankEnrollment",
            "Your account is already set up. Sign in with your password and authenticator code.");
    case Notice::PasswordTooShort:
        return policy.minLength > 0 ?
                    QCoreApplication::translate("PlankEnrollment",
                        "That password is too short. Use at least %1 characters.").arg(policy.minLength) :
                    QCoreApplication::translate("PlankEnrollment", "That password is too short.");
    case Notice::PasswordTooSimple:
        return policy.minClasses > 1 ?
                    QCoreApplication::translate("PlankEnrollment",
                        "That password is too simple. Mix at least %1 kinds of characters: lower-case and "
                        "upper-case letters, digits and symbols.").arg(policy.minClasses) :
                    QCoreApplication::translate("PlankEnrollment",
                        "That password is too simple. Mix lower-case and upper-case letters, digits and symbols.");
    case Notice::PasswordReused:
        return QCoreApplication::translate("PlankEnrollment",
            "You can't reuse an earlier password. Choose a new one.");
    case Notice::PasswordPolicy:
        return QCoreApplication::translate("PlankEnrollment",
            "The studio's password rules didn't accept that password. Choose a different one.");
    case Notice::CodeRejected:
        return QCoreApplication::translate("PlankEnrollment",
            "That code didn't work. Enter the current 6-digit code from your authenticator app. "
            "If it keeps failing, check that this computer sets its clock automatically.");
    case Notice::NextCodeRejected:
        return QCoreApplication::translate("PlankEnrollment",
            "That code didn't work. Wait until your authenticator app shows a new code, then enter it.");
    case Notice::PasskeyNotAdded:
        return QCoreApplication::translate("PlankEnrollment",
            "Touch ID couldn't be added to your account. You are signed in; next time, sign in "
            "with your password and authenticator code.");
    case Notice::PasskeyNotCreated:
    default:
        return QCoreApplication::translate("PlankEnrollment",
            "Touch ID couldn't be set up on this Mac. Try again, or skip this step.");
    }
}

QString passwordCheckText(PasswordCheck check, const PasswordPolicy& policy)
{
    switch (check) {
    case PasswordCheck::Ok:
        return QString();
    case PasswordCheck::Empty:
        return QCoreApplication::translate("PlankEnrollment", "Enter a new password.");
    case PasswordCheck::Mismatch:
        return QCoreApplication::translate("PlankEnrollment", "The two passwords don't match.");
    case PasswordCheck::TooLong:
        return QCoreApplication::translate("PlankEnrollment", "That password is too long.");
    case PasswordCheck::TooShort:
        return noticeText(Notice::PasswordTooShort, policy);
    case PasswordCheck::TooSimple:
    default:
        return noticeText(Notice::PasswordTooSimple, policy);
    }
}

Conversation::Conversation(PlankBrokerClient::Config config)
    : m_Client(std::move(config))
{
}

Conversation::~Conversation()
{
    QMutexLocker lock(&m_Lock);
    forgetSecrets();
    zero(m_ConversationId);
}

Step Conversation::step() const
{
    QMutexLocker lock(&m_Lock);
    return m_Step;
}

QString Conversation::username() const
{
    QMutexLocker lock(&m_Lock);
    return m_Username;
}

bool Conversation::hasConversation() const
{
    QMutexLocker lock(&m_Lock);
    return !m_ConversationId.isEmpty();
}

void Conversation::forgetSecrets()
{
    zero(m_Password);
    zero(m_Secret);
    zero(m_OtpauthUri);
}

Reply Conversation::send(Endpoint endpoint, QJsonObject body) const
{
    PlankBrokerClient::Response response = m_Client.post(path(endpoint), body);
    // The request copy of any secret in `body` goes with it.
    for (auto it = body.begin(); it != body.end(); ++it) *it = QString();
    Reply reply = parseReply(endpoint, response.status, response.body);
    response.body.fill('\0');
    throwForTransport(reply);
    return reply;
}

Outcome Conversation::deniedOutcome()
{
    const bool changed = m_PasswordChanged;
    const bool maybeChanged = m_PasswordMaybeChanged;
    finishLocked();
    m_PasswordMaybeChanged = false;
    m_Step = Step::Credentials;
    Outcome outcome;
    outcome.step = Step::Credentials;
    outcome.notice = changed ? Notice::DeniedAfterPasswordChange :
                     maybeChanged ? Notice::DeniedPasswordMaybeChanged : Notice::Denied;
    return outcome;
}

Outcome Conversation::authenticatorOutcome(Notice notice) const
{
    Outcome outcome;
    outcome.step = Step::Authenticator;
    outcome.notice = notice;
    outcome.otpauthUri = m_OtpauthUri;
    outcome.secret = m_Secret;
    return outcome;
}

Outcome Conversation::begin(const QString& username, QString oneTimePassword)
{
    QMutexLocker lock(&m_Lock);
    // A fresh start abandons any earlier conversation.
    finishLocked();
    m_PasswordChanged = false;
    m_PasswordMaybeChanged = false;
    m_Username = username.trimmed();
    if (m_Username.isEmpty() || m_Username.size() > 255 || oneTimePassword.isEmpty() ||
            oneTimePassword.size() > MaximumPasswordLength) {
        zero(oneTimePassword);
        return deniedOutcome();
    }

    QJsonObject start {{QStringLiteral("username"), m_Username}};
    const PlankBrokerClient::Config& config = m_Client.config();
    if (config.devicePublicKey) {
        // Same device binding as /v1/auth/start (section 14.2), optional.
        const QString deviceKey = config.devicePublicKey();
        if (PlankBroker::isDevicePublicKey(deviceKey)) start.insert(QStringLiteral("device_key"), deviceKey);
    }
    Reply reply;
    try {
        reply = send(Endpoint::Start, start);
    } catch (...) {
        zero(oneTimePassword);
        throw;
    }
    if (reply.state != State::Challenge) {
        zero(oneTimePassword);
        return deniedOutcome();
    }

    // Identical for every user name: one secret prompt. Anything this Client
    // cannot answer with the one-time password is a protocol error.
    QJsonArray responses;
    if (!PlankBroker::buildResponses(reply.challenge.prompts, m_Username, oneTimePassword, QString(), responses)) {
        zero(oneTimePassword);
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    m_ConversationId = reply.challenge.conversationId;
    try {
        reply = send(Endpoint::Respond, QJsonObject {
            {QStringLiteral("conversation_id"), m_ConversationId},
            {QStringLiteral("responses"), responses},
        });
    } catch (...) {
        for (int i = 0; i < responses.size(); ++i) responses[i] = QString();
        zero(oneTimePassword);
        throw;
    }
    for (int i = 0; i < responses.size(); ++i) responses[i] = QString();

    switch (reply.state) {
    case State::NewPassword:
        m_Password = oneTimePassword;
        zero(oneTimePassword);
        m_Policy = reply.policy;
        m_Step = Step::NewPassword;
        {
            Outcome outcome;
            outcome.step = Step::NewPassword;
            outcome.policy = m_Policy;
            return outcome;
        }
    case State::Totp:
        // Not expired (changed at a studio desk): straight to the authenticator.
        m_Password = oneTimePassword;
        zero(oneTimePassword);
        m_OtpauthUri = reply.otpauthUri;
        m_Secret = reply.secret;
        zero(reply.otpauthUri);
        zero(reply.secret);
        m_Step = Step::Authenticator;
        return authenticatorOutcome();
    case State::AlreadyEnrolled: {
        zero(oneTimePassword);
        finishLocked();
        m_Step = Step::Credentials;
        Outcome outcome;
        outcome.step = Step::Credentials;
        outcome.notice = Notice::AlreadyEnrolled;
        return outcome;
    }
    case State::Denied:
    default:
        zero(oneTimePassword);
        return deniedOutcome();
    }
}

Outcome Conversation::changePassword(QString newPassword)
{
    QMutexLocker lock(&m_Lock);
    if (m_Step != Step::NewPassword || m_ConversationId.isEmpty()) {
        zero(newPassword);
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    Reply reply;
    try {
        reply = send(Endpoint::Password, QJsonObject {
            {QStringLiteral("conversation_id"), m_ConversationId},
            {QStringLiteral("new_password"), newPassword},
        });
    } catch (const PlankBrokerError& error) {
        zero(newPassword);
        // Only these fail before the request is sent (or, for 429, before the
        // broker acts on it). Anything else, e.g. a timeout, may have lost the
        // reply to a change the broker already made.
        if (error.kind() != PlankBrokerError::RateLimited &&
                error.kind() != PlankBrokerError::NotConfigured &&
                error.kind() != PlankBrokerError::Tls) {
            m_PasswordMaybeChanged = true;
        }
        throw;
    } catch (...) {
        zero(newPassword);
        m_PasswordMaybeChanged = true;
        throw;
    }
    switch (reply.state) {
    case State::Totp:
        m_PasswordMaybeChanged = false;
        zero(m_Password);
        m_Password = newPassword;
        zero(newPassword);
        m_PasswordChanged = true;
        m_OtpauthUri = reply.otpauthUri;
        m_Secret = reply.secret;
        zero(reply.otpauthUri);
        zero(reply.secret);
        m_Step = Step::Authenticator;
        return authenticatorOutcome();
    case State::PasswordRejected: {
        // Still at the password stage: an earlier lost request changed nothing.
        m_PasswordMaybeChanged = false;
        zero(newPassword);
        Outcome outcome;
        outcome.step = Step::NewPassword;
        outcome.notice = rejectionNotice(reply.reason);
        outcome.policy = m_Policy;
        return outcome;
    }
    case State::Denied:
    default:
        zero(newPassword);
        return deniedOutcome();
    }
}

Outcome Conversation::verifyCode(QString code)
{
    QMutexLocker lock(&m_Lock);
    if (m_Step != Step::Authenticator || m_ConversationId.isEmpty()) {
        zero(code);
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    if (!PlankBroker::isValidOtp(code)) {
        zero(code);
        return authenticatorOutcome(Notice::CodeRejected);
    }
    Reply reply;
    try {
        reply = send(Endpoint::Totp, QJsonObject {
            {QStringLiteral("conversation_id"), m_ConversationId},
            {QStringLiteral("code"), code},
        });
    } catch (...) {
        zero(code);
        throw;
    }
    zero(code);
    switch (reply.state) {
    case State::Authenticated: {
        // The broker enabled the token and signed in with password + code.
        m_PasskeyAvailable = reply.passkeyAvailable;
        forgetSecrets();
        Outcome outcome;
        outcome.step = m_PasskeyAvailable ? Step::Passkey : Step::Done;
        outcome.passkeyAvailable = m_PasskeyAvailable;
        outcome.sessionToken = reply.session.sessionToken;
        outcome.username = reply.session.username.isEmpty() ? m_Username : reply.session.username;
        outcome.deviceBound = reply.session.deviceBound;
        reply.session.sessionToken.fill(QChar('\0'));
        m_Step = outcome.step;
        if (outcome.step == Step::Done) finishLocked();
        return outcome;
    }
    case State::Enrolled: {
        // Token enabled, but the follow-up login did not succeed (typically the
        // code's time step passed): the next code signs in normally.
        m_PasskeyAvailable = reply.passkeyAvailable;
        zero(m_Secret);
        zero(m_OtpauthUri);
        m_Step = Step::NextCode;
        Outcome outcome;
        outcome.step = Step::NextCode;
        outcome.passkeyAvailable = m_PasskeyAvailable;
        outcome.username = m_Username;
        return outcome;
    }
    case State::CodeRejected:
        return authenticatorOutcome(Notice::CodeRejected);
    case State::Denied:
    default:
        return deniedOutcome();
    }
}

Outcome Conversation::signInWithNextCode(QString code)
{
    QMutexLocker lock(&m_Lock);
    if (m_Step != Step::NextCode || m_Password.isEmpty()) {
        zero(code);
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    Outcome outcome;
    outcome.step = Step::NextCode;
    outcome.username = m_Username;
    outcome.passkeyAvailable = m_PasskeyAvailable;
    if (!PlankBroker::isValidOtp(code)) {
        zero(code);
        outcome.notice = Notice::NextCodeRejected;
        return outcome;
    }
    QJsonArray responses;
    try {
        PlankBroker::AuthReply reply = m_Client.start(m_Username);
        if (!PlankBroker::buildResponses(reply.prompts, m_Username, m_Password, code, responses)) {
            throw PlankBrokerError(PlankBrokerError::Protocol);
        }
        reply = m_Client.respond(reply.conversationId, responses);
        for (int i = 0; i < responses.size(); ++i) responses[i] = QString();
        zero(code);
        if (reply.kind != PlankBroker::ReplyKind::Authenticated) throw PlankBrokerError(PlankBrokerError::Denied);
        outcome.sessionToken = reply.sessionToken;
        reply.sessionToken.fill(QChar('\0'));
        if (!reply.username.isEmpty()) outcome.username = reply.username;
        outcome.deviceBound = reply.deviceBound;
    } catch (const PlankBrokerError& error) {
        for (int i = 0; i < responses.size(); ++i) responses[i] = QString();
        zero(code);
        if (error.kind() != PlankBrokerError::Denied) throw;
        outcome.notice = Notice::NextCodeRejected;
        return outcome;
    }
    zero(m_Password);
    outcome.step = m_PasskeyAvailable && !m_ConversationId.isEmpty() ? Step::Passkey : Step::Done;
    m_Step = outcome.step;
    if (outcome.step == Step::Done) finishLocked();
    return outcome;
}

Outcome Conversation::addPasskey(const QString& mapping)
{
    QMutexLocker lock(&m_Lock);
    if (m_Step != Step::Passkey || m_ConversationId.isEmpty() || !isPasskeyMapping(mapping)) {
        throw PlankBrokerError(PlankBrokerError::Protocol);
    }
    const Reply reply = send(Endpoint::Passkey, QJsonObject {
        {QStringLiteral("conversation_id"), m_ConversationId},
        {QStringLiteral("mapping"), mapping},
    });
    Outcome outcome;
    outcome.step = Step::Done;
    outcome.notice = reply.state == State::PasskeyAdded ? Notice::None : Notice::PasskeyNotAdded;
    m_Step = Step::Done;
    finishLocked();
    return outcome;
}

void Conversation::finish() noexcept
{
    QMutexLocker lock(&m_Lock);
    finishLocked();
}

void Conversation::finishLocked() noexcept
{
    forgetSecrets();
    if (m_ConversationId.isEmpty()) return;
    QString conversationId = m_ConversationId;
    zero(m_ConversationId);
    try {
        // The broker also drops it on its TTL; this is a courtesy.
        send(Endpoint::Finish, QJsonObject {{QStringLiteral("conversation_id"), conversationId}});
    } catch (...) {
    }
    zero(conversationId);
}

}

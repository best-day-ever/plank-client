#include "onboardingcontroller.h"

#include "backend/brokersessionstore.h"
#include "backend/onboardingstate.h"
#include "backend/plankpasskey.h"
#include "backend/qrencoder.h"
#include "remotebroker.h"
#include "settings/streamingpreferences.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QSettings>
#include <QThreadPool>

using PlankEnrollment::Notice;
using PlankEnrollment::Outcome;
using PlankEnrollment::Step;

namespace {

void zero(QString& value)
{
    value.fill(QChar('\0'));
    value.clear();
}

const char* decisionName(OnboardingState::Decision decision)
{
    switch (decision) {
    case OnboardingState::Decision::Show: return "show";
    case OnboardingState::Decision::Completed: return "completed";
    case OnboardingState::Decision::NotConfigured: return "broker not configured";
    case OnboardingState::Decision::RememberedSession: return "remembered session";
    case OnboardingState::Decision::LocalPasskeys: return "local Touch ID key";
    case OnboardingState::Decision::Bookmarks: return "saved workstations";
    case OnboardingState::Decision::RemoteHosts: return "remote display setups";
    default: return "unknown";
    }
}

}

OnboardingController::OnboardingController(RemoteBroker* broker, QObject* parent)
    : QObject(parent),
      m_Broker(broker)
{
    // Quitting must never wait on a courtesy request to the broker.
    if (QCoreApplication::instance() != nullptr) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() { m_Quitting = true; });
    }
}

OnboardingController::~OnboardingController()
{
    ++m_Generation;
    zero(m_Secret);
    zero(m_QrModules);
    // The Conversation zeroes its secrets when the last reference goes; the
    // broker drops the conversation on its TTL.
    m_Conversation.reset();
}

bool OnboardingController::shouldShowOnLaunch(StreamingPreferences* preferences)
{
    if (qEnvironmentVariableIntValue("PLANK_FORCE_ONBOARDING") == 1) return true;
    OnboardingState::Inputs inputs;
    QSettings settings;
    OnboardingState::readSettings(settings, inputs);
    inputs.brokerConfigured = preferences != nullptr &&
            !PlankBroker::normalizePins(preferences->brokerPins).isEmpty() &&
            PlankBroker::isEndpointName(preferences->brokerHost.trimmed()) &&
            preferences->brokerPort >= 1 && preferences->brokerPort <= 65535;
    // Cheapest first: the Keychain and the file system are only consulted
    // while nothing else has decided.
    if (OnboardingState::shouldShow(inputs)) {
        BrokerSessionStore::Saved saved;
        const QString address = QStringLiteral("%1:%2").arg(preferences->brokerHost).arg(preferences->brokerPort);
        inputs.rememberedSession = BrokerSessionStore::load(address, saved);
        saved.token.fill(QChar('\0'));
    }
    if (OnboardingState::shouldShow(inputs)) {
        inputs.localPasskeys = OnboardingState::hasLocalPasskeys(PlankPasskeyHelper::localStoreRoot());
    }
    const OnboardingState::Decision decision = OnboardingState::decide(inputs);
    qInfo() << "First sign-in wizard:" << decisionName(decision);
    return decision == OnboardingState::Decision::Show;
}

QString OnboardingController::step() const
{
    switch (m_Step) {
    case Step::Welcome: return QStringLiteral("welcome");
    case Step::Credentials: return QStringLiteral("credentials");
    case Step::NewPassword: return QStringLiteral("newPassword");
    case Step::Authenticator: return QStringLiteral("authenticator");
    case Step::NextCode: return QStringLiteral("nextCode");
    case Step::Passkey: return QStringLiteral("passkey");
    case Step::Done: default: return QStringLiteral("done");
    }
}

void OnboardingController::setStep(Step step)
{
    m_Step = step;
    if (step != Step::Authenticator) clearAuthenticator();
}

void OnboardingController::clearAuthenticator()
{
    zero(m_Secret);
    zero(m_QrModules);
    m_QrSize = 0;
}

void OnboardingController::endConversation()
{
    if (!m_Conversation) return;
    auto conversation = std::move(m_Conversation);
    m_Conversation.reset();
    if (m_Quitting || QCoreApplication::closingDown()) return;
    QThreadPool::globalInstance()->start([conversation]() { conversation->finish(); });
}

void OnboardingController::markCompleted()
{
    QSettings settings;
    OnboardingState::markCompleted(settings);
}

bool OnboardingController::touchIdPossible() const
{
    return m_Broker && !m_Broker->passkeyHelperProgram().isEmpty() &&
            PlankBroker::isPasskeyUsername(PlankBroker::normalizePasskeyUsername(m_Username));
}

void OnboardingController::reset()
{
    cancel();
    m_Username.clear();
    m_AlreadyEnrolled = false;
    m_PasskeyWarning = false;
    m_Policy = PlankEnrollment::PasswordPolicy();
    m_Step = Step::Welcome;
    emit changed();
}

void OnboardingController::cancel()
{
    ++m_Generation;
    endConversation();
    clearAuthenticator();
    m_BusyText.clear();
    m_ErrorText.clear();
    emit changed();
}

void OnboardingController::chooseSetUp()
{
    if (busy()) return;
    m_ErrorText.clear();
    m_AlreadyEnrolled = false;
    setStep(Step::Credentials);
    emit changed();
}

void OnboardingController::chooseExistingAccount()
{
    markCompleted();
    leaveToSignIn();
}

void OnboardingController::leaveToSignIn()
{
    m_SignInUsername = m_Username;
    cancel();
}

QString OnboardingController::takeSignInUsername()
{
    const QString username = m_SignInUsername;
    if (!m_SignInUsername.isEmpty()) {
        m_SignInUsername.clear();
        emit changed();
    }
    return username;
}

void OnboardingController::run(const QString& busyText, Work work)
{
    if (!m_Conversation) return;
    m_BusyText = busyText;
    m_ErrorText.clear();
    emit changed();
    const quint64 generation = ++m_Generation;
    const auto conversation = m_Conversation;
    QPointer<OnboardingController> self(this);
    QThreadPool::globalInstance()->start([self, generation, conversation, work = std::move(work)]() mutable {
        Outcome outcome;
        std::shared_ptr<PlankBrokerError> failure;
        try {
            outcome = work(*conversation);
        } catch (const PlankBrokerError& error) {
            failure = std::make_shared<PlankBrokerError>(error);
        }
        work = nullptr; // drops any secret the task captured
        QMetaObject::invokeMethod(qApp, [self, generation, outcome, failure]() mutable {
            if (!self || generation != self->m_Generation) {
                outcome.sessionToken.fill(QChar('\0'));
                outcome.secret.fill(QChar('\0'));
                return;
            }
            self->m_BusyText.clear();
            if (failure) {
                // Network, TLS, rate limiting, protocol: same screen, try again.
                self->m_ErrorText = failure->userMessage();
                emit self->changed();
                return;
            }
            self->apply(std::move(outcome));
        }, Qt::QueuedConnection);
    });
}

void OnboardingController::apply(Outcome outcome)
{
    if (outcome.step == Step::NewPassword) m_Policy = outcome.policy;
    m_ErrorText = PlankEnrollment::noticeText(outcome.notice, m_Policy);
    m_AlreadyEnrolled = outcome.notice == Notice::AlreadyEnrolled;
    m_PasskeyWarning = outcome.notice == Notice::PasskeyNotAdded;
    if (m_AlreadyEnrolled) {
        // Nothing left to set up on this account: the normal sign-in is next.
        markCompleted();
        m_SignInUsername = m_Username;
    }

    if (outcome.signedIn()) {
        if (m_Broker) m_Broker->adoptSession(outcome.sessionToken, outcome.username);
        zero(outcome.sessionToken);
        qInfo() << "First sign-in finished; session device-bound:" << outcome.deviceBound;
        markCompleted();
        m_SignInUsername.clear();
    }

    Step next = outcome.step;
    if (next == Step::Passkey && !touchIdPossible()) next = Step::Done;
    if (next == Step::Credentials || next == Step::Done) endConversation();
    if (next == Step::Done && m_Broker) m_Broker->refreshPasskeys();

    setStep(next);
    if (next == Step::Authenticator && !outcome.secret.isEmpty() && outcome.secret != m_Secret) {
        m_Secret = outcome.secret;
        const QrEncoder::Matrix matrix = QrEncoder::encode(outcome.otpauthUri.toUtf8(), QrEncoder::Ecc::Medium);
        m_QrModules.clear();
        m_QrSize = 0;
        if (matrix.isValid()) {
            m_QrSize = matrix.size;
            m_QrModules.reserve(matrix.modules.size());
            for (const bool dark : matrix.modules) m_QrModules.append(dark ? QLatin1Char('1') : QLatin1Char('0'));
        }
    }
    zero(outcome.secret);
    zero(outcome.otpauthUri);
    emit changed();
}

void OnboardingController::submitCredentials(const QString& username, QString oneTimePassword)
{
    if (busy()) {
        zero(oneTimePassword);
        return;
    }
    if (!m_Broker || !m_Broker->configured()) {
        zero(oneTimePassword);
        m_ErrorText = PlankBrokerError(PlankBrokerError::NotConfigured).userMessage();
        emit changed();
        return;
    }
    const QString user = username.trimmed();
    if (user.isEmpty() || oneTimePassword.isEmpty()) {
        zero(oneTimePassword);
        m_ErrorText = tr("Enter your username and the one-time password from the studio.");
        emit changed();
        return;
    }
    endConversation();
    m_Username = user;
    m_AlreadyEnrolled = false;
    m_Conversation = std::make_shared<PlankEnrollment::Conversation>(m_Broker->brokerClientConfig());
    run(tr("Checking your account..."), [user, password = std::move(oneTimePassword)](
            PlankEnrollment::Conversation& conversation) mutable {
        return conversation.begin(user, std::move(password));
    });
}

void OnboardingController::submitNewPassword(QString password, QString confirmation)
{
    if (busy() || m_Step != Step::NewPassword) {
        zero(password);
        zero(confirmation);
        return;
    }
    const PlankEnrollment::PasswordCheck check = PlankEnrollment::checkNewPassword(password, confirmation, m_Policy);
    zero(confirmation);
    if (check != PlankEnrollment::PasswordCheck::Ok) {
        zero(password);
        m_ErrorText = PlankEnrollment::passwordCheckText(check, m_Policy);
        emit changed();
        return;
    }
    run(tr("Saving your new password..."), [password = std::move(password)](
            PlankEnrollment::Conversation& conversation) mutable {
        return conversation.changePassword(std::move(password));
    });
}

void OnboardingController::submitCode(QString code)
{
    if (busy() || m_Step != Step::Authenticator) return;
    if (!PlankBroker::isValidOtp(code)) {
        m_ErrorText = tr("Enter the 6-digit code from your authenticator app.");
        emit changed();
        return;
    }
    run(tr("Checking the code..."), [code = std::move(code)](PlankEnrollment::Conversation& conversation) mutable {
        return conversation.verifyCode(std::move(code));
    });
}

void OnboardingController::submitNextCode(QString code)
{
    if (busy() || m_Step != Step::NextCode) return;
    if (!PlankBroker::isValidOtp(code)) {
        m_ErrorText = tr("Enter the 6-digit code from your authenticator app.");
        emit changed();
        return;
    }
    run(tr("Signing in..."), [code = std::move(code)](PlankEnrollment::Conversation& conversation) mutable {
        return conversation.signInWithNextCode(std::move(code));
    });
}

void OnboardingController::copySecret()
{
    if (m_Step != Step::Authenticator || m_Secret.isEmpty()) return;
    if (QClipboard* clipboard = QGuiApplication::clipboard()) clipboard->setText(m_Secret);
}

void OnboardingController::setUpTouchId()
{
    if (busy() || m_Step != Step::Passkey || !touchIdPossible()) return;
    const QString program = m_Broker->passkeyHelperProgram();
    const QString rpId = m_Broker->passkeyRelyingParty();
    const QString user = PlankBroker::normalizePasskeyUsername(m_Username);
    run(tr("Setting up Touch ID..."), [program, rpId, user](PlankEnrollment::Conversation& conversation) {
        const PlankPasskeyHelper helper(program);
        PlankPasskeyHelper::CreatedKey key;
        if (!helper.create(rpId, user, key) || !PlankEnrollment::isPasskeyMapping(key.mapping)) {
            Outcome outcome;
            outcome.step = Step::Passkey;
            outcome.notice = Notice::PasskeyNotCreated;
            return outcome;
        }
        Outcome outcome;
        try {
            outcome = conversation.addPasskey(key.mapping);
        } catch (...) {
            // Unknown whether it was stored: keep no key IPA may not know.
            helper.remove(rpId, user);
            throw;
        }
        if (outcome.notice == Notice::PasskeyNotAdded) helper.remove(rpId, user);
        return outcome;
    });
}

void OnboardingController::skipTouchId()
{
    if (busy() || m_Step != Step::Passkey) return;
    m_ErrorText.clear();
    endConversation();
    setStep(Step::Done);
    emit changed();
}

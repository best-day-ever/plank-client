#pragma once

#include "backend/plankenrollment.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class RemoteBroker;
class StreamingPreferences;

// "Welcome to BDE Fernweh": the first sign-in wizard, exposed to QML as the
// Onboarding singleton (OnboardingView.qml). A person whose administrator
// created the account (with a one-time password and remote access) signs in
// with it, chooses a new password, adds an authenticator app from a QR code,
// optionally Touch ID, and ends signed in exactly as after a normal sign-in.
// Protocol: PlankEnrollment (bde-linux docs/plank-broker.md section 16).
//
// Broker calls run on the global thread pool, one at a time, with the same
// generation guard as RemoteBroker; passwords are zeroed after use and the
// authenticator secret is dropped as soon as the step is left.
class OnboardingController : public QObject
{
    Q_OBJECT
    // welcome, credentials, newPassword, authenticator, nextCode, passkey,
    // displays (client only: shown instead of done while this computer's
    // screens have no display setup yet), done
    Q_PROPERTY(QString step READ step NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString busyText READ busyText NOTIFY changed)
    // Generic, Client-side text for the current screen (never broker text).
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    // Set when the account turned out to be set up already: offer sign-in.
    Q_PROPERTY(bool alreadyEnrolled READ alreadyEnrolled NOTIFY changed)
    Q_PROPERTY(QString username READ username NOTIFY changed)
    Q_PROPERTY(int passwordMinLength READ passwordMinLength NOTIFY changed)
    Q_PROPERTY(int passwordMinClasses READ passwordMinClasses NOTIFY changed)
    Q_PROPERTY(QString secretGroups READ secretGroups NOTIFY changed)
    // QR code of the otpauth URI: qrSize modules per side, qrModules a
    // row-major string of '1' (dark) and '0'.
    Q_PROPERTY(int qrSize READ qrSize NOTIFY changed)
    Q_PROPERTY(QString qrModules READ qrModules NOTIFY changed)
    // The Done screen's note (Touch ID not added) is a warning, not an error.
    Q_PROPERTY(bool passkeyWarning READ passkeyWarning NOTIFY changed)
    // User name to prefill in the normal sign-in after leaving the wizard.
    Q_PROPERTY(QString signInUsername READ signInUsername NOTIFY changed)

public:
    explicit OnboardingController(RemoteBroker* broker, QObject* parent = nullptr);
    ~OnboardingController() override;

    // main.cpp: open the wizard on this launch? (OnboardingState rules.)
    static bool shouldShowOnLaunch(StreamingPreferences* preferences);

    Q_INVOKABLE void reset();
    Q_INVOKABLE void chooseSetUp();
    // "I already have an authenticator app": never offer the wizard again.
    Q_INVOKABLE void chooseExistingAccount();
    Q_INVOKABLE void submitCredentials(const QString& username, QString oneTimePassword);
    Q_INVOKABLE void submitNewPassword(QString password, QString confirmation);
    Q_INVOKABLE void submitCode(QString code);
    Q_INVOKABLE void submitNextCode(QString code);
    Q_INVOKABLE void copySecret();
    Q_INVOKABLE void setUpTouchId();
    Q_INVOKABLE void skipTouchId();
    // Leaves the displays step (saved or skipped) for done.
    Q_INVOKABLE void finishDisplays();
    // Leave to the normal sign-in (keeps signInUsername for the prefill).
    Q_INVOKABLE void leaveToSignIn();
    // The wizard view is going away: end any conversation, forget secrets.
    Q_INVOKABLE void cancel();
    // Returns and forgets signInUsername.
    Q_INVOKABLE QString takeSignInUsername();

    QString step() const;
    bool busy() const { return !m_BusyText.isEmpty(); }
    QString busyText() const { return m_BusyText; }
    QString errorText() const { return m_ErrorText; }
    bool alreadyEnrolled() const { return m_AlreadyEnrolled; }
    QString username() const { return m_Username; }
    int passwordMinLength() const { return m_Policy.minLength; }
    int passwordMinClasses() const { return m_Policy.minClasses; }
    QString secretGroups() const { return PlankEnrollment::groupSecret(m_Secret); }
    int qrSize() const { return m_QrSize; }
    QString qrModules() const { return m_QrModules; }
    bool passkeyWarning() const { return m_PasskeyWarning; }
    QString signInUsername() const { return m_SignInUsername; }

signals:
    void changed();

private:
    using Work = std::function<PlankEnrollment::Outcome(PlankEnrollment::Conversation&)>;
    void run(const QString& busyText, Work work);
    void apply(PlankEnrollment::Outcome outcome);
    void setStep(PlankEnrollment::Step step);
    void clearAuthenticator();
    void endConversation();
    void markCompleted();
    bool touchIdPossible() const;
    static bool displaysNeedSetup();

    QPointer<RemoteBroker> m_Broker;
    std::shared_ptr<PlankEnrollment::Conversation> m_Conversation;
    quint64 m_Generation = 0;
    bool m_Quitting = false;

    PlankEnrollment::Step m_Step = PlankEnrollment::Step::Welcome;
    QString m_BusyText;
    QString m_ErrorText;
    bool m_AlreadyEnrolled = false;
    QString m_Username;
    PlankEnrollment::PasswordPolicy m_Policy;
    QString m_Secret;
    int m_QrSize = 0;
    QString m_QrModules;
    bool m_PasskeyWarning = false;
    QString m_SignInUsername;
    // Done was reached and the screens still need a display setup.
    bool m_DisplaysPending = false;
};

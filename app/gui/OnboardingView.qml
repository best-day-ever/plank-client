import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0
import Onboarding 1.0
import RemoteBroker 1.0

// "Welcome to BDE Fernweh": first sign-in for a person whose administrator
// created the account and issued a one-time password. The steps come from
// the Onboarding controller (bde-linux docs/plank-broker.md section 16):
// welcome -> credentials -> newPassword -> authenticator -> [nextCode] ->
// [passkey] -> [displays] -> done. The displays step is client-only: this
// computer's screens get a display setup (DisplaySetup) unless they have one.
// Leaving the view ends the setup conversation.
Item {
    id: onboardingView
    objectName: qsTr("Welcome")

    PlankTheme {
        id: theme
    }

    readonly property string step: Onboarding.step

    Component.onCompleted: Onboarding.reset()
    Component.onDestruction: Onboarding.cancel()

    StackView.onActivated: focusFirstField()
    onStepChanged: {
        if (step === "displays") {
            DisplaySetup.begin("", "", "onboarding")
        }
        focusFirstField()
    }

    function focusFirstField() {
        if (step === "credentials") {
            usernameField.forceActiveFocus()
        } else if (step === "newPassword") {
            newPasswordField.forceActiveFocus()
        } else if (step === "authenticator") {
            codeField.forceActiveFocus()
        } else if (step === "nextCode") {
            nextCodeField.forceActiveFocus()
        }
    }

    function leave() {
        if (StackView.view) {
            StackView.view.pop()
        }
    }

    function submitCredentials() {
        if (!credentialsButton.enabled) {
            return
        }
        Onboarding.submitCredentials(usernameField.text, oneTimePasswordField.text)
        oneTimePasswordField.clear()
    }

    function submitNewPassword() {
        if (!newPasswordButton.enabled) {
            return
        }
        Onboarding.submitNewPassword(newPasswordField.text, repeatPasswordField.text)
        newPasswordField.clear()
        repeatPasswordField.clear()
    }

    function submitCode() {
        if (!codeButton.enabled) {
            return
        }
        Onboarding.submitCode(codeField.text)
        codeField.clear()
    }

    function submitNextCode() {
        if (!nextCodeButton.enabled) {
            return
        }
        Onboarding.submitNextCode(nextCodeField.text)
        nextCodeField.clear()
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        Item {
            width: parent.width
            implicitHeight: panel.height + 64

            Rectangle {
                id: panel
                anchors.horizontalCenter: parent.horizontalCenter
                y: 32
                width: Math.min(onboardingView.step === "displays" ? 760 : 500, onboardingView.width - 32)
                height: column.implicitHeight + 56
                color: theme.surface
                radius: theme.radiusLarge
                border.width: 1
                border.color: theme.borderSubtle

                ColumnLayout {
                    id: column
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 28
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: {
                            switch (onboardingView.step) {
                            case "welcome": return qsTr("Welcome to BDE fernweh")
                            case "credentials": return qsTr("Set up your account")
                            case "newPassword": return qsTr("Choose your password")
                            case "authenticator": return qsTr("Add your authenticator app")
                            case "nextCode": return qsTr("One more code")
                            case "passkey": return qsTr("Use Touch ID on this Mac?")
                            case "displays": return qsTr("Your screens")
                            default: return qsTr("You're all set")
                            }
                        }
                        color: theme.textPrimary
                        font.pointSize: 17
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }

                    // ------------------------------------------------ welcome
                    ColumnLayout {
                        visible: onboardingView.step === "welcome"
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("BDE fernweh connects you to your studio workstation from wherever you are. Setting up takes about two minutes: you choose your password and add an authenticator app on your phone.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        Button {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            text: qsTr("Set up my account")
                            highlighted: true
                            onClicked: Onboarding.chooseSetUp()
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Choose this if the studio gave you a one-time password.")
                            color: theme.textDisabled
                            font.pointSize: 10
                            wrapMode: Text.Wrap
                        }
                        Button {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            text: qsTr("I already have an authenticator app")
                            flat: true
                            onClicked: {
                                Onboarding.chooseExistingAccount()
                                onboardingView.leave()
                            }
                        }
                    }

                    // -------------------------------------------- credentials
                    ColumnLayout {
                        visible: onboardingView.step === "credentials"
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Enter your studio username and the one-time password the studio gave you.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                            Layout.bottomMargin: 4
                        }
                        Label {
                            text: qsTr("Username")
                            color: theme.textSecondary
                        }
                        PlankTextField {
                            id: usernameField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            Keys.onReturnPressed: oneTimePasswordField.forceActiveFocus()
                            Keys.onEnterPressed: oneTimePasswordField.forceActiveFocus()
                        }
                        Label {
                            text: qsTr("One-time password")
                            color: theme.textSecondary
                            Layout.topMargin: 4
                        }
                        PlankTextField {
                            id: oneTimePasswordField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            Keys.onReturnPressed: onboardingView.submitCredentials()
                            Keys.onEnterPressed: onboardingView.submitCredentials()
                        }
                    }

                    // ------------------------------------------- new password
                    ColumnLayout {
                        visible: onboardingView.step === "newPassword"
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Your one-time password has done its job. Choose the password you will use from now on, here and at the studio.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: Onboarding.passwordMinLength > 0 || Onboarding.passwordMinClasses > 1
                            text: {
                                var hints = []
                                if (Onboarding.passwordMinLength > 0) {
                                    hints.push(qsTr("at least %1 characters").arg(Onboarding.passwordMinLength))
                                }
                                if (Onboarding.passwordMinClasses > 1) {
                                    hints.push(qsTr("at least %1 kinds of characters (lower-case, upper-case, digits, symbols)").arg(Onboarding.passwordMinClasses))
                                }
                                return qsTr("Use %1.").arg(hints.join(qsTr(" and ")))
                            }
                            color: theme.textDisabled
                            font.pointSize: 10
                            wrapMode: Text.Wrap
                            Layout.bottomMargin: 4
                        }
                        Label {
                            text: qsTr("New password")
                            color: theme.textSecondary
                        }
                        PlankTextField {
                            id: newPasswordField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            Keys.onReturnPressed: repeatPasswordField.forceActiveFocus()
                            Keys.onEnterPressed: repeatPasswordField.forceActiveFocus()
                        }
                        Label {
                            text: qsTr("Repeat the new password")
                            color: theme.textSecondary
                            Layout.topMargin: 4
                        }
                        PlankTextField {
                            id: repeatPasswordField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            echoMode: TextInput.Password
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            Keys.onReturnPressed: onboardingView.submitNewPassword()
                            Keys.onEnterPressed: onboardingView.submitNewPassword()
                        }
                    }

                    // ------------------------------------------ authenticator
                    ColumnLayout {
                        visible: onboardingView.step === "authenticator"
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Open an authenticator app on your phone (for example Google Authenticator, Microsoft Authenticator or 1Password), add an account and scan this code.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        OnboardingQrCode {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 250
                            Layout.preferredHeight: 250
                            modules: Onboarding.qrSize
                            pattern: Onboarding.qrModules
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Can't scan it? Enter this key in the app instead:")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: theme.spaceMedium
                            Label {
                                Layout.fillWidth: true
                                text: Onboarding.secretGroups
                                color: theme.textPrimary
                                font.family: "Menlo"
                                font.pointSize: 13
                                wrapMode: Text.Wrap
                            }
                            Button {
                                id: copyButton
                                property bool copied: false
                                text: copied ? qsTr("Copied") : qsTr("Copy")
                                flat: true
                                onClicked: {
                                    Onboarding.copySecret()
                                    copied = true
                                    copiedTimer.restart()
                                }
                                Timer {
                                    id: copiedTimer
                                    interval: 2000
                                    onTriggered: copyButton.copied = false
                                }
                            }
                        }
                        Label {
                            text: qsTr("Then enter the 6-digit code the app shows")
                            color: theme.textSecondary
                            Layout.topMargin: 4
                        }
                        PlankTextField {
                            id: codeField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            placeholderText: qsTr("6 digits")
                            maximumLength: 6
                            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                            font.letterSpacing: 2
                            Keys.onReturnPressed: onboardingView.submitCode()
                            Keys.onEnterPressed: onboardingView.submitCode()
                        }
                    }

                    // ----------------------------------------------- next code
                    ColumnLayout {
                        visible: onboardingView.step === "nextCode"
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Your authenticator app is set up. To finish signing in, wait until it shows a new code and enter that one.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        Label {
                            text: qsTr("Authenticator code")
                            color: theme.textSecondary
                            Layout.topMargin: 4
                        }
                        PlankTextField {
                            id: nextCodeField
                            Layout.fillWidth: true
                            enabled: !Onboarding.busy
                            placeholderText: qsTr("6 digits")
                            maximumLength: 6
                            inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                            font.letterSpacing: 2
                            Keys.onReturnPressed: onboardingView.submitNextCode()
                            Keys.onEnterPressed: onboardingView.submitNextCode()
                        }
                    }

                    // ------------------------------------------------- passkey
                    Label {
                        visible: onboardingView.step === "passkey"
                        Layout.fillWidth: true
                        text: qsTr("You are signed in. Next time you can sign in with your fingerprint instead of your password and a code. The sign-in key stays on this Mac.")
                        color: theme.textSecondary
                        wrapMode: Text.Wrap
                    }

                    // ------------------------------------------------ displays
                    ColumnLayout {
                        visible: onboardingView.step === "displays"
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Your workstation will show its desktop on these screens, each at its exact size and arranged like your desk. Turn a screen off to keep it for this Mac, or pick another size. You can change this any time in Settings.")
                            color: theme.textSecondary
                            wrapMode: Text.Wrap
                        }
                        DisplaySetupPanel {
                            Layout.fillWidth: true
                            compact: true
                        }
                    }

                    // ---------------------------------------------------- done
                    Label {
                        visible: onboardingView.step === "done"
                        Layout.fillWidth: true
                        text: RemoteBroker.signedIn ?
                                  qsTr("You are signed in as %1. Your workstations are next.").arg(RemoteBroker.username) :
                                  qsTr("Your account is ready.")
                        color: theme.textSecondary
                        wrapMode: Text.Wrap
                    }

                    // ----------------------------------------- shared footer
                    Label {
                        visible: Onboarding.errorText !== ""
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        text: Onboarding.errorText
                        color: Onboarding.passkeyWarning ? theme.warning : theme.danger
                        wrapMode: Text.Wrap
                    }

                    RowLayout {
                        visible: onboardingView.step !== "welcome"
                        Layout.fillWidth: true
                        Layout.topMargin: 10
                        spacing: theme.spaceMedium

                        BusyIndicator {
                            visible: Onboarding.busy
                            running: visible
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                        }
                        Label {
                            text: Onboarding.busyText
                            color: theme.textSecondary
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }

                        // Secondary actions per step.
                        Button {
                            visible: onboardingView.step === "credentials"
                            text: Onboarding.alreadyEnrolled ? qsTr("Go to sign-in") : qsTr("Back")
                            flat: true
                            enabled: !Onboarding.busy
                            onClicked: {
                                if (Onboarding.alreadyEnrolled) {
                                    Onboarding.leaveToSignIn()
                                    onboardingView.leave()
                                } else {
                                    Onboarding.reset()
                                }
                            }
                        }
                        Button {
                            visible: onboardingView.step === "nextCode"
                            text: qsTr("Sign in another way")
                            flat: true
                            enabled: !Onboarding.busy
                            onClicked: {
                                Onboarding.leaveToSignIn()
                                onboardingView.leave()
                            }
                        }
                        Button {
                            visible: onboardingView.step === "passkey"
                            text: qsTr("Not now")
                            flat: true
                            enabled: !Onboarding.busy
                            onClicked: Onboarding.skipTouchId()
                        }
                        Button {
                            visible: onboardingView.step === "displays"
                            text: qsTr("Skip for now")
                            flat: true
                            onClicked: Onboarding.finishDisplays()
                        }

                        // Primary action per step.
                        Button {
                            id: credentialsButton
                            visible: onboardingView.step === "credentials"
                            text: qsTr("Continue")
                            highlighted: true
                            enabled: !Onboarding.busy && RemoteBroker.configured &&
                                     usernameField.text.trim().length > 0 && oneTimePasswordField.text.length > 0
                            onClicked: onboardingView.submitCredentials()
                        }
                        Button {
                            id: newPasswordButton
                            visible: onboardingView.step === "newPassword"
                            text: qsTr("Save password")
                            highlighted: true
                            enabled: !Onboarding.busy && newPasswordField.text.length > 0 &&
                                     repeatPasswordField.text.length > 0
                            onClicked: onboardingView.submitNewPassword()
                        }
                        Button {
                            id: codeButton
                            visible: onboardingView.step === "authenticator"
                            text: qsTr("Verify")
                            highlighted: true
                            enabled: !Onboarding.busy && codeField.text.length === 6
                            onClicked: onboardingView.submitCode()
                        }
                        Button {
                            id: nextCodeButton
                            visible: onboardingView.step === "nextCode"
                            text: qsTr("Sign in")
                            highlighted: true
                            enabled: !Onboarding.busy && nextCodeField.text.length === 6
                            onClicked: onboardingView.submitNextCode()
                        }
                        Button {
                            visible: onboardingView.step === "passkey"
                            text: qsTr("Use Touch ID")
                            highlighted: true
                            enabled: !Onboarding.busy
                            onClicked: Onboarding.setUpTouchId()
                        }
                        Button {
                            visible: onboardingView.step === "displays"
                            text: qsTr("Use this layout")
                            highlighted: true
                            enabled: DisplaySetup.planOk
                            onClicked: {
                                if (DisplaySetup.accept()) {
                                    Onboarding.finishDisplays()
                                }
                            }
                        }
                        Button {
                            visible: onboardingView.step === "done"
                            text: qsTr("Show my workstations")
                            highlighted: true
                            onClicked: onboardingView.leave()
                        }
                    }
                }
            }
        }
    }
}

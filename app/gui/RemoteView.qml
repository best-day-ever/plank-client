import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import RemoteBroker 1.0
import ComputerManager 1.0
import DisplaySetup 1.0
import Onboarding 1.0
import StreamingPreferences 1.0

// Remote (broker) mode: sign in once with username, password and
// authenticator code, pick an assigned workstation, stream through the
// broker's lease (bde-linux docs/plank-broker.md sections 10.1/10.2).
// A user with a Touch ID key on this Mac (section 13.4) signs in with it
// instead; without one, or when it does not work, password + code is asked.
Item {
    id: remoteView
    objectName: qsTr("Remote")

    PlankTheme {
        id: theme
    }

    property string errorText: ""
    // Set after a Touch ID fallback or by "Use password instead".
    property bool usePassword: false
    readonly property bool passkeyReady: !usePassword &&
        RemoteBroker.passkeyUsers.indexOf(usernameField.text.trim().toLowerCase()) >= 0

    StackView.onActivated: {
        RemoteBroker.initialize(ComputerManager)
        RemoteBroker.refreshPasskeys()
        // Back from the first sign-in wizard without a session: continue
        // with the user name typed there.
        var prefill = Onboarding.takeSignInUsername()
        if (RemoteBroker.signedIn) {
            RemoteBroker.refreshHosts()
        } else if (prefill !== "") {
            usernameField.text = prefill
            if (remoteView.passkeyReady) {
                usernameField.forceActiveFocus()
            } else {
                passwordField.forceActiveFocus()
            }
        } else {
            usernameField.forceActiveFocus()
        }
    }

    Connections {
        target: RemoteBroker
        function onErrorOccurred(message) {
            remoteView.errorText = message
        }
        function onPasskeyFallback(message) {
            remoteView.usePassword = true
            remoteView.errorText = message
            passwordField.forceActiveFocus()
        }
        function onConnectReady(hostName) {
            remoteView.errorText = ""
            var session = RemoteBroker.takeSession()
            if (session === null) {
                return
            }
            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {
                                                   "appName": hostName,
                                                   "session": session,
                                                   "isResume": false
                                               })
            stackView.push(segue)
        }
        function onDisplaySetupRequired(hostId, hostName, reason) {
            // New screens: the display setup. A fixed layout that no longer
            // fits (older workstation, remote Mac): its manual page.
            screensDialog.openFor(hostId, hostName, reason, true, reason === "new-screens" ? 0 : 1)
        }
        function onStreamSetupRequired(hostId, hostName, reason) {
            displaySetupDialog.openFor(hostId, hostName, true, "", reason)
        }
        function onTouchIdSetupRequested() {
            if (RemoteBroker.touchIdSetupPending) touchIdSetupDialog.open()
        }
        function onTouchIdSetupCompleted() {
            touchIdSetupDialog.close()
        }
        function onStateChanged() {
            if (!RemoteBroker.signedIn && touchIdSetupDialog.visible) touchIdSetupDialog.close()
        }
    }

    NavigableDialog {
        id: touchIdSetupDialog
        title: qsTr("Sign in with Touch ID?")
        width: Math.min(440, remoteView.width - 32)
        height: Math.min(implicitHeight, remoteView.height - 20)
        modal: true
        closePolicy: Popup.NoAutoClose
        standardButtons: Dialog.NoButton

        ColumnLayout {
            width: parent.width
            spacing: theme.spaceMedium
            Label {
                Layout.fillWidth: true
                text: qsTr("You are signed in. Add a sign-in key to this Mac so you can use Touch ID next time. Your authenticator app stays available as a fallback.")
                color: theme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: RemoteBroker.touchIdSetupError !== ""
                text: RemoteBroker.touchIdSetupError
                color: theme.danger
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: qsTr("Not now")
                    enabled: !RemoteBroker.touchIdSetupBusy
                    onClicked: {
                        RemoteBroker.skipTouchIdAfterSignIn()
                        touchIdSetupDialog.close()
                    }
                }
                Item { Layout.fillWidth: true }
                BusyIndicator {
                    visible: RemoteBroker.touchIdSetupBusy
                    running: visible
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                }
                Button {
                    text: qsTr("Set up Touch ID")
                    highlighted: true
                    enabled: !RemoteBroker.touchIdSetupBusy
                    onClicked: RemoteBroker.setUpTouchIdAfterSignIn()
                }
            }
        }
    }

    // Per-workstation settings (display setup and stream quality), asked on
    // first connect and editable from each row; kept locally
    // (RemoteBroker.displaySetup / streamSetup and their save functions).
    NavigableDialog {
        id: displaySetupDialog
        property string hostId: ""
        property string hostName: ""
        property bool connectAfter: false
        property string reason: ""
        // DisplaySetup.hostSummary: mode (follow, custom, legacy), summary.
        property var screens: ({})
        property string streamReason: ""
        property var stream: ({})
        title: qsTr("Settings for %1").arg(hostName)
        width: Math.min(560, remoteView.width - 40)
        height: Math.min(implicitHeight, remoteView.height - 20)
        dim: false
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        function openFor(hostId, hostName, connectAfter, reason, streamReason) {
            displaySetupDialog.streamReason = streamReason || ""
            displaySetupDialog.hostId = hostId
            displaySetupDialog.hostName = hostName
            displaySetupDialog.connectAfter = connectAfter
            displaySetupDialog.reason = reason
            displaySetupDialog.loadScreens()
            displaySetupDialog.loadStream()
            displaySetupDialog.open()
        }

        function loadScreens() {
            screens = DisplaySetup.hostSummary(hostId)
            screensFollow.checked = screens.mode !== "custom"
        }

        function loadStream() {
            stream = RemoteBroker.streamSetup(hostId)
            streamVideoSettings.hostPlatform = stream.platform || 0
            // A choice the workstation refused is shown as the user's own
            // choice, so it can be changed rather than hidden behind defaults.
            streamUseDefaults.checked = stream.useDefaults === true && streamReason === ""
            streamVideoSettings.load(stream.captureSource, stream.videoProfile,
                                     stream.officeBitratesKbps, stream.internetBitratesKbps)
        }

        function streamProblem(capture, profile) {
            return RemoteBroker.streamProfileProblem(hostId, capture, profile)
        }

        function saveStream() {
            return RemoteBroker.saveStreamSetup(hostId, streamUseDefaults.checked,
                                                streamVideoSettings.captureSource,
                                                streamVideoSettings.videoProfile,
                                                streamVideoSettings.officeBitratesKbps,
                                                streamVideoSettings.internetBitratesKbps)
        }

        onOpened: {
            // Right after the workstation refused a choice (streamReason) its
            // capabilities were just probed, so an unusable choice cannot be
            // saved. Otherwise they come from an earlier connect and may be
            // stale (host upgraded, encoder fixed): the choice is only
            // flagged, and the connect checks it against the live host.
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return streamReason === "" || streamVideoSettings.problemText === ""
            })
        }

        onAccepted: {
            var chosenMode = screensCustom.checked ? "custom" : "follow"
            if (screens.mode !== chosenMode)
                DisplaySetup.setHostMode(hostId, chosenMode)
            if (!saveStream()) {
                remoteView.errorText = qsTr("Those stream settings are not supported.")
                return
            }
            if (connectAfter) {
                remoteView.errorText = ""
                RemoteBroker.connectToHost(hostId)
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            // ------------------------------------------------------- screens
            Label {
                text: qsTr("Screens")
                font.bold: true
                font.pointSize: 13
            }
            Label {
                Layout.fillWidth: true
                text: displaySetupDialog.screens.summary || ""
                wrapMode: Text.Wrap
                opacity: 0.72
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceMedium

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true

                    RadioButton {
                        id: screensFollow
                        text: qsTr("Use my display setup")
                    }
                    RadioButton {
                        id: screensCustom
                        text: qsTr("Custom for this workstation")
                        checked: !screensFollow.checked
                    }
                    Label {
                        visible: displaySetupDialog.screens.mode === "legacy"
                        Layout.fillWidth: true
                        text: qsTr("This workstation uses a fixed layout. Saving a display setup for it replaces the fixed layout.")
                        wrapMode: Text.Wrap
                        opacity: 0.72
                        font.pointSize: 10
                    }
                }
                Button {
                    text: qsTr("Screens…")
                    Layout.alignment: Qt.AlignTop
                    onClicked: {
                        var hostId = displaySetupDialog.hostId
                        var hostName = displaySetupDialog.hostName
                        displaySetupDialog.close()
                        screensDialog.openFor(hostId, hostName, "", false,
                                              displaySetupDialog.screens.mode === "legacy" ? 1 : 0)
                    }
                }
            }

            // ------------------------------------------------ stream quality
            Label {
                Layout.topMargin: 8
                text: qsTr("Stream quality")
                font.bold: true
                font.pointSize: 13
            }
            Label {
                Layout.fillWidth: true
                visible: displaySetupDialog.streamReason !== ""
                text: displaySetupDialog.streamReason
                color: theme.warning
                wrapMode: Text.Wrap
            }
            PlankCheckBox {
                id: streamUseDefaults
                Layout.fillWidth: true
                text: qsTr("Use the remote access defaults (Settings › Remote Access)")
                onToggled: {
                    if (checked) {
                        var defaults = displaySetupDialog.stream.defaults
                        streamVideoSettings.load(defaults.captureSource, defaults.videoProfile,
                                                 defaults.officeBitratesKbps, defaults.internetBitratesKbps)
                    }
                }
            }
            PlankVideoSettings {
                id: streamVideoSettings
                Layout.fillWidth: true
                enabled: !streamUseDefaults.checked
                showRoutes: true
                profileProblem: displaySetupDialog.streamProblem
            }
            Label {
                Layout.fillWidth: true
                visible: displaySetupDialog.streamReason === "" && streamVideoSettings.problemText !== ""
                text: qsTr("This is what the workstation reported on the last connect. Connecting checks it again.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }
        }
    }

    function submitSignIn() {
        if (!signInButton.enabled) {
            return
        }
        remoteView.errorText = ""
        if (remoteView.passkeyReady) {
            RemoteBroker.signInWithPasskey(usernameField.text)
            return
        }
        RemoteBroker.signIn(usernameField.text, passwordField.text, otpField.text)
        passwordField.clear()
        otpField.clear()
    }

    // ---------------------------------------------------------------- sign in
    Rectangle {
        id: signInPanel
        visible: !RemoteBroker.signedIn
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 32)
        height: signInColumn.implicitHeight + 48
        color: theme.surface
        radius: theme.radiusLarge
        border.width: 1
        border.color: theme.borderSubtle

        ColumnLayout {
            id: signInColumn
            anchors.fill: parent
            anchors.margins: 24
            spacing: 8

            Label {
                text: qsTr("Remote access")
                color: theme.textPrimary
                font.pointSize: 15
                font.weight: Font.DemiBold
                Layout.fillWidth: true
            }
            Label {
                text: remoteView.passkeyReady ?
                          qsTr("Sign in with Touch ID. This Mac holds your sign-in key.") :
                          qsTr("Sign in with your studio account and the code from your authenticator app.")
                color: theme.textSecondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                text: RemoteBroker.brokerAddress
                color: theme.textDisabled
                font.pointSize: 9
                Layout.fillWidth: true
                Layout.bottomMargin: 4
            }

            Label {
                text: qsTr("Username")
                color: theme.textSecondary
                Layout.fillWidth: true
            }
            PlankTextField {
                id: usernameField
                Layout.fillWidth: true
                enabled: !RemoteBroker.busy
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                onTextChanged: remoteView.usePassword = false
                Keys.onReturnPressed: remoteView.passkeyReady ? remoteView.submitSignIn() : passwordField.forceActiveFocus()
                Keys.onEnterPressed: remoteView.passkeyReady ? remoteView.submitSignIn() : passwordField.forceActiveFocus()
            }

            Label {
                visible: !remoteView.passkeyReady
                text: qsTr("Password")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: passwordField
                visible: !remoteView.passkeyReady
                Layout.fillWidth: true
                enabled: !RemoteBroker.busy
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                Keys.onReturnPressed: otpField.forceActiveFocus()
                Keys.onEnterPressed: otpField.forceActiveFocus()
            }

            Label {
                visible: !remoteView.passkeyReady
                text: qsTr("Authenticator code")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: otpField
                visible: !remoteView.passkeyReady
                Layout.fillWidth: true
                enabled: !RemoteBroker.busy
                placeholderText: qsTr("6 digits")
                maximumLength: 6
                inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                font.letterSpacing: 2
                Keys.onReturnPressed: remoteView.submitSignIn()
                Keys.onEnterPressed: remoteView.submitSignIn()
            }

            Label {
                visible: !RemoteBroker.configured
                text: qsTr("Remote access is not configured. Add the broker key (SPKI SHA-256 pin) in Settings.")
                color: theme.warning
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            Label {
                visible: remoteView.errorText !== "" && !RemoteBroker.signedIn
                text: remoteView.errorText
                color: theme.danger
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 4
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: theme.spaceMedium

                BusyIndicator {
                    visible: RemoteBroker.busy
                    running: visible
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                }
                Label {
                    text: RemoteBroker.busyText
                    color: theme.textSecondary
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Button {
                    visible: remoteView.passkeyReady
                    text: qsTr("Use password instead")
                    flat: true
                    enabled: !RemoteBroker.busy
                    onClicked: {
                        remoteView.usePassword = true
                        passwordField.forceActiveFocus()
                    }
                }
                Button {
                    id: signInButton
                    text: remoteView.passkeyReady ? qsTr("Sign in with Touch ID") : qsTr("Sign in")
                    highlighted: true
                    enabled: !RemoteBroker.busy && RemoteBroker.configured &&
                             usernameField.text.length > 0 &&
                             (remoteView.passkeyReady ||
                              (passwordField.text.length > 0 && otpField.text.length === 6))
                    onClicked: remoteView.submitSignIn()
                }
            }

            // First sign-in (one-time password from the studio): the wizard.
            Button {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                visible: RemoteBroker.configured
                text: qsTr("New to BDE fernweh? Set up your account")
                flat: true
                enabled: !RemoteBroker.busy
                onClicked: {
                    remoteView.errorText = ""
                    stackView.push("qrc:/gui/OnboardingView.qml")
                }
            }
        }
    }

    // ------------------------------------------------------------- host list
    ColumnLayout {
        visible: RemoteBroker.signedIn
        anchors.fill: parent
        anchors.margins: 24
        spacing: theme.spaceMedium

        RowLayout {
            Layout.fillWidth: true
            spacing: theme.spaceMedium

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: qsTr("Signed in as %1").arg(RemoteBroker.username)
                    color: theme.textPrimary
                    font.pointSize: 13
                    font.weight: Font.DemiBold
                    elide: Label.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: RemoteBroker.brokerAddress
                    color: theme.textDisabled
                    font.pointSize: 9
                    Layout.fillWidth: true
                }
            }
            BusyIndicator {
                visible: RemoteBroker.busy
                running: visible
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }
            Label {
                visible: RemoteBroker.busy
                text: RemoteBroker.busyText
                color: theme.textSecondary
            }
            Button {
                text: qsTr("Refresh")
                enabled: !RemoteBroker.busy
                onClicked: {
                    remoteView.errorText = ""
                    RemoteBroker.refreshHosts()
                }
            }
            Button {
                text: qsTr("Sign out")
                onClicked: {
                    remoteView.errorText = ""
                    RemoteBroker.logout()
                }
            }
        }

        Label {
            visible: remoteView.errorText !== ""
            text: remoteView.errorText
            color: theme.danger
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            visible: hostList.count === 0 && !RemoteBroker.busy
            text: qsTr("No workstations are assigned to you for remote access.")
            color: theme.textSecondary
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        ListView {
            id: hostList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: RemoteBroker.hosts

            delegate: Rectangle {
                width: hostList.width
                height: 68
                radius: theme.radiusMedium
                color: theme.surface
                border.width: 1
                border.color: theme.borderSubtle

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    spacing: theme.spaceMedium

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: !modelData.online ? theme.textDisabled :
                               (modelData.inUseBy !== "" ? theme.warning : theme.success)
                        Layout.alignment: Qt.AlignVCenter
                    }

                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Label {
                            text: modelData.name
                            color: theme.textPrimary
                            font.pointSize: 12
                            font.weight: Font.Medium
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: {
                                var parts = []
                                parts.push(modelData.online ? qsTr("Online") : qsTr("Offline"))
                                if (modelData.inUseBy !== "") {
                                    parts.push(qsTr("in use by %1").arg(modelData.inUseBy))
                                }
                                if (!modelData.connectable && modelData.reason !== "") {
                                    parts.push(modelData.reason)
                                }
                                return parts.join(" · ")
                            }
                            color: theme.textSecondary
                            font.pointSize: 10
                            elide: Label.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Button {
                        text: qsTr("Settings…")
                        flat: true
                        enabled: !RemoteBroker.busy
                        onClicked: displaySetupDialog.openFor(modelData.id, modelData.name, false, "")
                    }

                    Button {
                        text: qsTr("Connect")
                        highlighted: modelData.connectable
                        enabled: modelData.connectable && !RemoteBroker.busy
                        onClicked: {
                            remoteView.errorText = ""
                            RemoteBroker.connectToHost(modelData.id)
                        }
                    }
                }
            }
        }
    }
}

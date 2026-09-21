import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import RemoteBroker 1.0
import ComputerManager 1.0

// Remote (broker) mode: sign in once with username, password and
// authenticator code, pick an assigned workstation, stream through the
// broker's lease (bde-linux docs/plank-broker.md sections 10.1/10.2).
Item {
    id: remoteView
    objectName: qsTr("Remote")

    PlankTheme {
        id: theme
    }

    property string errorText: ""

    StackView.onActivated: {
        RemoteBroker.initialize(ComputerManager)
        if (RemoteBroker.signedIn) {
            RemoteBroker.refreshHosts()
        } else {
            usernameField.forceActiveFocus()
        }
    }

    Connections {
        target: RemoteBroker
        function onErrorOccurred(message) {
            remoteView.errorText = message
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
    }

    function submitSignIn() {
        if (!signInButton.enabled) {
            return
        }
        remoteView.errorText = ""
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
                text: qsTr("Sign in with your studio account and the code from your authenticator app.")
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
                Keys.onReturnPressed: passwordField.forceActiveFocus()
                Keys.onEnterPressed: passwordField.forceActiveFocus()
            }

            Label {
                text: qsTr("Password")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: passwordField
                Layout.fillWidth: true
                enabled: !RemoteBroker.busy
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                Keys.onReturnPressed: otpField.forceActiveFocus()
                Keys.onEnterPressed: otpField.forceActiveFocus()
            }

            Label {
                text: qsTr("Authenticator code")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: otpField
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
                    id: signInButton
                    text: qsTr("Sign in")
                    highlighted: true
                    enabled: !RemoteBroker.busy && RemoteBroker.configured &&
                             usernameField.text.length > 0 && passwordField.text.length > 0 &&
                             otpField.text.length === 6
                    onClicked: remoteView.submitSignIn()
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

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0
import RemoteBroker 1.0

// The display setup wizard: which of this computer's screens the workstation
// uses, at what size, and where; with the manual setup one click away. Opened
// on the first connect with screens that have no setup yet ("new-screens"),
// from Settings > Displays, from a workstation's Settings… ("Screens…"), and
// from the "screens changed" banner. Saving stores the layout for this set
// of monitors, for every workstation or for one; connectAfter then connects.
NavigableDialog {
    id: dialog

    property string hostId: ""
    property string hostName: ""
    property string reason: ""
    property bool connectAfter: false
    // 0: the overview, 1: the manual setup.
    property int page: 0

    title: hostName !== "" ? qsTr("Screens for %1").arg(hostName) : qsTr("Display setup")
    width: Math.min(860, parent.width - 40)
    height: Math.min(parent.height - 40, 860)
    modal: true
    dim: true
    closePolicy: Popup.CloseOnEscape

    function openFor(hostId, hostName, reason, connectAfter, page) {
        dialog.hostId = hostId || ""
        dialog.hostName = hostName || ""
        dialog.reason = reason || ""
        dialog.connectAfter = connectAfter === true
        dialog.page = page || 0
        DisplaySetup.begin(dialog.hostId, dialog.hostName, dialog.reason)
        dialog.open()
    }

    function save() {
        if (!DisplaySetup.accept()) {
            return
        }
        dialog.close()
        if (dialog.connectAfter && dialog.hostId !== "") {
            RemoteBroker.connectToHost(dialog.hostId)
        }
    }

    Connections {
        target: DisplaySetup
        function onActionRequested(action, key) {
            if (!dialog.visible) {
                return
            }
            if (action === "choose-screens") {
                dialog.page = 1
            } else if (action === "update-host") {
                updateHint.visible = true
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: theme.spaceMedium

        PlankTheme {
            id: theme
        }

        Label {
            Layout.fillWidth: true
            visible: text !== ""
            text: {
                if (dialog.reason === "new-screens") {
                    return qsTr("These screens (%1) are new to BDE Fernweh. Check how the workstation will show them; you can change it later in Settings.").arg(DisplaySetup.label)
                }
                if (dialog.reason === "onboarding" || dialog.reason === "") {
                    return ""
                }
                return dialog.reason
            }
            color: dialog.reason === "new-screens" ? theme.textSecondary : theme.warning
            wrapMode: Text.Wrap
        }

        Label {
            id: updateHint
            Layout.fillWidth: true
            visible: false
            text: qsTr("Ask your studio administrator to update PLANK on this workstation. Until then it uses the older layouts.")
            color: theme.textSecondary
            wrapMode: Text.Wrap
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: dialog.page

            ScrollView {
                id: overviewScroll
                clip: true
                contentWidth: availableWidth

                DisplaySetupPanel {
                    width: overviewScroll.availableWidth
                }
            }

            ScrollView {
                id: advancedScroll
                clip: true
                contentWidth: availableWidth

                DisplayAdvancedPage {
                    width: advancedScroll.availableWidth
                    hostId: dialog.hostId
                    hostName: dialog.hostName
                    onFixedLayoutSaved: {
                        dialog.close()
                        if (dialog.connectAfter && dialog.hostId !== "") {
                            RemoteBroker.connectToHost(dialog.hostId)
                        }
                    }
                }
            }
        }
    }

    footer: Pane {
        padding: 12
        background: Rectangle {
            color: theme.surfaceRaised
        }

        PlankTheme {
            id: footerTheme
        }

        RowLayout {
            anchors.fill: parent
            spacing: footerTheme.spaceMedium

            Button {
                text: dialog.page === 0 ? qsTr("Advanced…") : qsTr("Overview")
                flat: true
                onClicked: dialog.page = dialog.page === 0 ? 1 : 0
            }

            PlankComboBox {
                visible: dialog.hostId !== ""
                Layout.preferredWidth: 260
                model: [qsTr("Use for every workstation"), qsTr("Only for %1").arg(dialog.hostName)]
                currentIndex: DisplaySetup.scope === "workstation" ? 1 : 0
                onActivated: function(index) { DisplaySetup.scope = index === 1 ? "workstation" : "global" }
            }

            Item {
                Layout.fillWidth: true
            }

            Button {
                text: qsTr("Cancel")
                flat: true
                onClicked: {
                    DisplaySetup.cancel()
                    dialog.close()
                }
            }
            Button {
                text: dialog.connectAfter ? qsTr("Save and connect") : qsTr("Save")
                highlighted: true
                enabled: DisplaySetup.planOk
                onClicked: dialog.save()
            }
        }
    }
}

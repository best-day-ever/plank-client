import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.15

NavigableDialog {
    id: dialog
    title: qsTr("Workstation in use")
    width: Math.min(560, parent.width - 40)
    modal: true
    focus: true
    property string text
    property bool signOutAvailable: false
    signal chosen(int choice)

    PlankTheme { id: collaborationTheme }

    contentItem: Label {
        objectName: "collaborationExplanation"
        text: dialog.text
        color: collaborationTheme.textPrimary
        wrapMode: Text.Wrap
    }

    footer: DialogButtonBox {
        leftPadding: dialog.padding - 6
        rightPadding: dialog.padding - 6
        topPadding: 4
        bottomPadding: 10
        Material.foreground: collaborationTheme.textPrimary
        Material.accent: collaborationTheme.accent
        background: Item {}
        Button {
            objectName: "cancelCollaboration"
            text: qsTr("Cancel")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Button {
            objectName: "signOutDesktop"
            visible: dialog.signOutAvailable
            text: qsTr("Sign out instead…")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: { dialog.chosen(1); dialog.close() }
        }
        Button {
            objectName: "requestCollaboration"
            text: qsTr("Request to join")
            highlighted: true
            flat: false
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: { dialog.chosen(2); dialog.close() }
        }
    }
    onRejected: chosen(-1)
}

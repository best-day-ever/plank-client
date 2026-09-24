import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

NavigableToolButton {
    id: control
    objectName: "plankVersionButton"
    property string version
    property string changelog

    PlankTheme { id: theme }

    text: version
    leftPadding: theme.spaceSmall
    rightPadding: theme.spaceSmall
    font.pointSize: 9
    font.weight: Font.Medium
    Accessible.name: qsTr("Version %1. Show changelog").arg(version)

    contentItem: Text {
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: control.hovered || control.visualFocus ? theme.textPrimary : theme.textSecondary
        verticalAlignment: Text.AlignVCenter
    }

    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: qsTr("What's new")
    onClicked: changelogDialog.open()

    NavigableDialog {
        id: changelogDialog
        objectName: "changelogDialog"
        title: qsTr("What's new in PLANK")
        modal: true
        dim: false
        focus: true
        width: Math.max(0, Math.min(660, parent.width - 32))
        height: Math.max(0, Math.min(640, parent.height - 32))
        standardButtons: Dialog.Close
        closePolicy: Popup.CloseOnEscape

        onOpened: {
            scroll.contentItem.contentY = 0
            notes.forceActiveFocus()
        }
        onClosed: control.forceActiveFocus()

        contentItem: ColumnLayout {
            spacing: theme.spaceMedium

            Label {
                objectName: "changelogInstalledVersion"
                text: qsTr("Installed Client: %1").arg(control.version)
                textFormat: Text.PlainText
                color: theme.textSecondary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            ScrollView {
                id: scroll
                objectName: "changelogScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                TextArea {
                    id: notes
                    objectName: "changelogNotes"
                    text: control.changelog || qsTr("No release notes are available for this build.")
                    textFormat: TextEdit.MarkdownText
                    wrapMode: TextEdit.Wrap
                    readOnly: true
                    selectByMouse: true
                    color: theme.textPrimary
                    font.pointSize: 11
                    padding: 0
                    rightPadding: theme.spaceMedium
                    background: null
                }
            }
        }
    }
}

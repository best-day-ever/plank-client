import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

NavigableDialog {
    id: dialog
    property string endpoint
    property string previousKey
    property string replacementKey
    title: qsTr("Host identity changed")
    modal: true
    dim: false
    width: Math.min(600, parent.width - 40)
    closePolicy: Popup.CloseOnEscape
    onOpened: {
        details.checked = false
        cancelButton.forceActiveFocus(Qt.TabFocus)
    }

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 12
        Label {
            text: dialog.endpoint
            textFormat: Text.PlainText
            font.bold: true
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("This workstation is presenting a different identity. This can happen after reinstalling or replacing the Host, but could also indicate an intercepted connection.")
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("No login credentials have been sent. Continue only if you trust this replacement Host.")
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Button {
            id: details
            text: checked ? qsTr("Hide details") : qsTr("Show details")
            checkable: true
            flat: true
        }
        Label {
            visible: details.checked
            text: qsTr("Previous public key:\n%1\n\nReplacement public key:\n%2").arg(dialog.previousKey).arg(dialog.replacementKey)
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
    }
    footer: DialogButtonBox {
        Button {
            id: cancelButton
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            Keys.onReturnPressed: dialog.reject()
            Keys.onEnterPressed: dialog.reject()
        }
        Button {
            text: qsTr("Trust Replacement Host")
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        onAccepted: dialog.accept()
        onRejected: dialog.reject()
    }
}

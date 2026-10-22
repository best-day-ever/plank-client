import QtQuick 2.0
import QtQuick.Controls 2.2

NavigableMessageDialog {
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: qsTr("Active BDE fernweh session")
    text: qsTr("This workstation already has an active BDE fernweh session. Disconnect the existing client and continue?")
    standardButtons: Dialog.Yes | Dialog.No
    acceptButtonText: qsTr("Take Over")
    rejectButtonText: qsTr("Cancel")
}

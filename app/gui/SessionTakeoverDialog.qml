import QtQuick 2.0
import QtQuick.Controls 2.2

NavigableMessageDialog {
    modal: true
    closePolicy: Popup.CloseOnEscape
    title: qsTr("Active PLANK session")
    text: qsTr("This workstation already has an active PLANK session. Disconnect the existing client and continue?")
    standardButtons: Dialog.Yes | Dialog.No
    acceptButtonText: qsTr("Take Over")
    rejectButtonText: qsTr("Cancel")
}

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

Rectangle {
    id: banner
    property string message: ""
    signal dismissed()
    visible: message !== ""
    implicitHeight: row.implicitHeight + 24
    color: theme.surfaceRaised
    radius: theme.radiusLarge
    border.color: theme.warning
    border.width: 1
    PlankTheme { id: theme }
    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 12
        spacing: theme.spaceMedium
        Label {
            objectName: "brokerSessionNotice"
            text: banner.message
            textFormat: Text.PlainText
            color: theme.textPrimary
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Accessible.role: Accessible.AlertMessage
        }
        Button {
            text: qsTr("Dismiss")
            flat: true
            onClicked: banner.dismissed()
        }
    }
}

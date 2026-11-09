import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

Rectangle {
    id: card
    property var host
    property string statusText
    property bool busy: false
    readonly property var people: host.connectedUsers || []
    signal settingsRequested()
    signal connectRequested()
    implicitHeight: Math.max(68, content.implicitHeight + 24)
    height: implicitHeight
    radius: theme.radiusMedium
    color: theme.surface
    border.width: 1
    border.color: theme.borderSubtle

    PlankTheme { id: theme }
    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 12
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: theme.spaceMedium
        Rectangle {
            width: 10; height: 10; radius: 5
            color: !card.host.online ? theme.textDisabled :
                   (!card.host.sharedDesktop && (card.host.busy || card.people.length > 0 || card.host.inUseBy !== "") ? theme.warning : theme.success)
            Layout.alignment: Qt.AlignVCenter
        }
        ColumnLayout {
            spacing: 3
            Layout.fillWidth: true
            Label {
                text: card.host.name
                color: theme.textPrimary
                font.pointSize: 12
                font.weight: Font.Medium
                elide: Label.ElideRight
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
            Label {
                text: card.statusText
                color: theme.textSecondary
                font.pointSize: 10
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
            Label {
                objectName: "connectedPeople"
                visible: card.people.length > 0
                text: qsTr("Connected (%1): %2").arg(card.people.length).arg(card.people.join(", "))
                color: theme.textPrimary
                font.pointSize: 10
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                textFormat: Text.PlainText
            }
        }
        Button {
            text: qsTr("Settings…")
            flat: true
            enabled: !card.busy
            onClicked: card.settingsRequested()
        }
        Button {
            objectName: "connectButton"
            text: qsTr("Connect")
            highlighted: card.host.connectable
            enabled: card.host.connectable && !card.busy
            onClicked: card.connectRequested()
        }
    }
}

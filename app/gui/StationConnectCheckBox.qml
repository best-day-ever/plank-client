import QtQuick 2.9
import QtQuick.Controls 2.2

CheckBox {
    id: control

    StationConnectTheme {
        id: theme
    }

    spacing: 10
    implicitHeight: 32

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        radius: 3
        color: control.checked ? theme.accent :
               (control.hovered ? theme.surfacePressed : theme.surfaceRaised)
        border.width: 1
        border.color: control.visualFocus ? theme.accent :
                      (control.checked ? theme.accent : theme.border)

        Text {
            anchors.centerIn: parent
            text: "✓"
            visible: control.checked
            color: "white"
            font.pixelSize: 13
            font.weight: Font.Bold
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.enabled ? theme.textPrimary : theme.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}

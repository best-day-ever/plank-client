import QtQuick 2.9
import QtQuick.Controls 2.2

GroupBox {
    id: control

    PlankTheme {
        id: theme
    }

    padding: 16
    leftPadding: 20
    topPadding: 43
    font.pointSize: 11

    label: Label {
        x: control.leftPadding
        y: 13
        text: control.title
        color: theme.textPrimary
        font.pointSize: 12
        font.weight: Font.DemiBold
    }

    background: Rectangle {
        color: theme.surface
        radius: theme.radiusMedium

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 3
            color: theme.accent
        }

        Rectangle {
            x: control.leftPadding
            y: 36
            width: parent.width - control.leftPadding - control.rightPadding
            height: 1
            color: theme.borderSubtle
        }
    }
}

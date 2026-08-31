import QtQuick 2.9
import QtQuick.Controls 2.2

GroupBox {
    id: control

    StationConnectTheme {
        id: theme
    }

    padding: 18
    topPadding: 32
    font.pointSize: 11

    label: Label {
        x: control.leftPadding
        y: 9
        text: control.title
        color: theme.textPrimary
        font.pointSize: 11
        font.weight: Font.DemiBold
    }

    background: Rectangle {
        color: theme.surface
        radius: theme.radiusMedium
        border.width: 1
        border.color: theme.borderSubtle
    }
}

import QtQuick 2.9
import QtQuick.Controls 2.2

TextField {
    id: control

    StationConnectTheme {
        id: theme
    }

    leftPadding: 12
    rightPadding: 12
    implicitHeight: 38
    color: theme.textPrimary
    placeholderTextColor: theme.textDisabled
    selectionColor: theme.accent
    selectedTextColor: "white"

    background: Rectangle {
        color: theme.surfaceRaised
        radius: theme.radiusSmall
        border.width: 1
        border.color: control.activeFocus ? theme.accent : theme.border
    }
}

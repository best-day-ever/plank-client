import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

ToolButton {
    id: control
    property string iconSource

    StationConnectTheme {
        id: theme
    }

    activeFocusOnTab: true

    icon.source: iconSource
    icon.width: 24
    icon.height: 24
    icon.color: theme.textPrimary

    background: Rectangle {
        implicitWidth: 44
        implicitHeight: 44
        radius: theme.radiusSmall
        color: control.down ? theme.surfacePressed :
               (control.hovered || control.visualFocus ? theme.surfaceHover : "transparent")
        border.width: control.visualFocus ? 1 : 0
        border.color: theme.accent
    }

    // This determines the size of the Material highlight. We increase it
    // from the default because we use larger than normal icons for TV readability.
    Layout.preferredHeight: parent.height

    Keys.onReturnPressed: {
        clicked()
    }

    Keys.onEnterPressed: {
        clicked()
    }

    Keys.onRightPressed: {
        nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
    }

    Keys.onLeftPressed: {
        nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    }
}

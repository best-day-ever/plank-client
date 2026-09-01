import QtQuick 2.0
import QtQuick.Controls 2.2

Menu {
    id: control
    property var initiator

    PlankTheme {
        id: theme
    }

    padding: 6

    background: Rectangle {
        implicitWidth: 220
        color: theme.surfaceRaised
        radius: theme.radiusMedium
        border.width: 1
        border.color: theme.border
    }

    onOpened: {
        // If the initiating object currently has keyboard focus,
        // give focus to the first visible and enabled menu item
        if (initiator.focus) {
            for (var i = 0; i < count; i++) {
                var item = itemAt(i)
                if (item.visible && item.enabled) {
                    item.forceActiveFocus(Qt.TabFocusReason)
                    break
                }
            }
        }
    }
}

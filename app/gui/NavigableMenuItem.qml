import QtQuick 2.0
import QtQuick.Controls 2.2

MenuItem {
    // StationConnect menus pass the owning menu explicitly. This keeps menu
    // dismissal deterministic across Qt versions and asynchronous Loaders.
    property Menu parentMenu

    // Ensure focus can't be given to an invisible item
    enabled: visible
    height: visible ? implicitHeight : 0
    focusPolicy: visible ? Qt.TabFocus : Qt.NoFocus

    onTriggered: {
        // We must close the context menu first or
        // it can steal focus from any dialogs that
        // onTriggered may spawn.
        parentMenu.close()
    }

    Keys.onReturnPressed: {
        triggered()
    }

    Keys.onEnterPressed: {
        triggered()
    }

    Keys.onEscapePressed: {
        parentMenu.close()
    }
}

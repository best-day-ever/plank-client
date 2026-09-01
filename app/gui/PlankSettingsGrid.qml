import QtQuick 2.9
import QtQuick.Layouts 1.2

GridLayout {
    id: control

    PlankTheme {
        id: theme
    }

    anchors.fill: parent
    columns: 2
    columnSpacing: theme.spaceLarge
    rowSpacing: 10
}

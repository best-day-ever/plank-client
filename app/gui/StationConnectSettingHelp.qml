import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Label {
    StationConnectTheme {
        id: theme
    }

    Layout.column: 1
    Layout.fillWidth: true
    color: theme.textSecondary
    font.pointSize: 9
    wrapMode: Text.Wrap
}

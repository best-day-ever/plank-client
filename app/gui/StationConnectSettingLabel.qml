import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Label {
    StationConnectTheme {
        id: theme
    }

    Layout.preferredWidth: 180
    Layout.minimumWidth: 180
    Layout.maximumWidth: 180
    Layout.alignment: Qt.AlignVCenter
    color: theme.textPrimary
    font.pointSize: 11
    wrapMode: Text.Wrap
}

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Label {
    StationConnectTheme {
        id: theme
    }

    Layout.preferredWidth: 280
    Layout.minimumWidth: 280
    Layout.maximumWidth: 280
    Layout.alignment: Qt.AlignVCenter
    color: theme.textPrimary
    font.pointSize: 10
    wrapMode: Text.Wrap
}

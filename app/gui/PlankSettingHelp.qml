import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

Label {
    PlankTheme {
        id: theme
    }

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    color: theme.textSecondary
    font.pointSize: 9
    wrapMode: Text.Wrap
}

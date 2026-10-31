import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import InternetConnectivity 1.0

Control {
    id: control
    objectName: "internetStatus"
    required property var monitor
    visible: monitor.state !== InternetConnectivity.Disabled
    hoverEnabled: true
    padding: 6
    implicitWidth: implicitContentWidth + leftPadding + rightPadding
    implicitHeight: implicitContentHeight + topPadding + bottomPadding
    Accessible.role: Accessible.StaticText
    Accessible.name: monitor.statusText
    Accessible.description: monitor.detail

    PlankTheme { id: theme }
    contentItem: RowLayout {
        spacing: 6
        Rectangle {
            implicitWidth: 7; implicitHeight: 7; radius: 3.5
            color: control.monitor.state === InternetConnectivity.Reachable ? theme.success :
                   control.monitor.state === InternetConnectivity.Checking ? theme.textSecondary : theme.warning
        }
        Label {
            objectName: "internetStatusLabel"
            text: control.monitor.statusText
            textFormat: Text.PlainText
            color: theme.textSecondary
            font.pointSize: 9
        }
    }
    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: monitor.detail
}

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import InternetConnectivity 1.0

Control {
    id: control
    objectName: "internetStatus"
    required property var monitor
    readonly property color statusColor: monitor.state === InternetConnectivity.Reachable ? theme.success :
                                         monitor.state === InternetConnectivity.NoResponse ? theme.danger :
                                         monitor.state === InternetConnectivity.Unavailable ? theme.warning : theme.textSecondary
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
            implicitWidth: 9; implicitHeight: 9; radius: 4.5
            color: control.statusColor
        }
        Label {
            objectName: "internetStatusLabel"
            text: control.monitor.statusText
            textFormat: Text.PlainText
            color: control.statusColor
            font.pointSize: 12
            font.weight: Font.DemiBold
        }
    }
    ToolTip.visible: hovered
    ToolTip.delay: 500
    ToolTip.text: monitor.detail
}

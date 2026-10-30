import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SystemProperties 1.0

ColumnLayout {
    id: root
    property string selection: "current"
    visible: SystemProperties.clientDisplaySelectionAvailable
    spacing: 6

    function refresh() {
        choice.model = SystemProperties.clientDisplayChoices(selection)
        for (var i = 0; i < choice.count; ++i) {
            if (choice.valueAt(i) === selection) {
                choice.currentIndex = i
                return
            }
        }
    }
    onSelectionChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: SystemProperties
        function onClientDisplaysChanged() { root.refresh() }
    }
    Label {
        text: qsTr("Client Displays")
        font.bold: true
    }
    PlankComboBox {
        id: choice
        objectName: "clientDisplayChoice"
        Layout.fillWidth: true
        textRole: "text"
        valueRole: "value"
        onActivated: root.selection = currentValue
        delegate: ItemDelegate {
            required property var modelData
            required property int index
            width: choice.width
            text: modelData.text
            enabled: modelData.enabled
            highlighted: choice.highlightedIndex === index
        }
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Choose where this session appears fullscreen. Match Client uses only these displays.")
        wrapMode: Text.Wrap
        opacity: 0.72
    }
}

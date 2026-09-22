import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0
import RemoteBroker 1.0

// Manual display setup for advanced users: exact sizes and positions (drag
// with edge snapping), which workstation output backs each display, how the
// stream is presented, and fixed layouts for workstations whose PLANK
// predates display arrangements (or remote Macs).
ColumnLayout {
    id: page

    property string hostId: ""
    property string hostName: ""
    // A fixed layout was saved for hostId (the dialog then closes).
    signal fixedLayoutSaved()

    spacing: theme.spaceMedium

    PlankTheme {
        id: theme
    }

    function backingIndex(preference) {
        return preference === "physical" ? 1 : preference === "virtual" ? 2 : 0
    }

    // ------------------------------------------------------ hand placement
    PlankCheckBox {
        Layout.fillWidth: true
        text: qsTr("Place the displays by hand (otherwise they are arranged like your desk)")
        checked: DisplaySetup.manual
        onToggled: DisplaySetup.setManual(checked)
    }

    Rectangle {
        id: canvas
        Layout.fillWidth: true
        Layout.preferredHeight: 240
        color: theme.canvas
        radius: theme.radiusMedium
        border.width: 1
        border.color: theme.borderSubtle

        readonly property var desk: DisplaySetup.desktop
        // Room around the desktop so a display can be dragged past its edges.
        readonly property real scale: Math.min((width - 80) / Math.max(1, desk.width),
                                               (height - 60) / Math.max(1, desk.height))
        readonly property real originX: (width - desk.width * scale) / 2
        readonly property real originY: (height - desk.height * scale) / 2

        Label {
            x: 10
            y: 6
            text: qsTr("Drag to move; edges snap together")
            color: theme.textDisabled
            font.pointSize: 9
        }

        Repeater {
            id: tiles
            model: DisplaySetup.monitors

            delegate: Rectangle {
                id: tile
                visible: modelData.included
                x: canvas.originX + modelData.x * canvas.scale
                y: canvas.originY + modelData.y * canvas.scale
                width: modelData.width * canvas.scale
                height: modelData.height * canvas.scale
                radius: theme.radiusSmall
                color: dragArea.drag.active ? theme.surfacePressed : theme.surfaceRaised
                border.width: modelData.primary ? 2 : 1
                border.color: modelData.primary ? theme.accent : theme.border

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    text: (modelData.primary ? "★ " : "") + modelData.name + "\n" + modelData.sizeText
                    color: theme.textPrimary
                    font.pointSize: 8
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: dragArea
                    anchors.fill: parent
                    drag.target: tile
                    cursorShape: Qt.SizeAllCursor
                    onReleased: {
                        // Back to workstation pixels, then snap each edge to a
                        // neighbour's edge within about 12 screen pixels.
                        var x = Math.round((tile.x - canvas.originX) / canvas.scale)
                        var y = Math.round((tile.y - canvas.originY) / canvas.scale)
                        var w = modelData.width
                        var h = modelData.height
                        var snap = 12 / canvas.scale
                        var others = DisplaySetup.monitors
                        for (var i = 0; i < others.length; ++i) {
                            var o = others[i]
                            if (!o.included || o.key === modelData.key) continue
                            var candidatesX = [o.x + o.width, o.x - w, o.x, o.x + o.width - w]
                            for (var a = 0; a < candidatesX.length; ++a) {
                                if (Math.abs(x - candidatesX[a]) < snap) { x = candidatesX[a]; break }
                            }
                            var candidatesY = [o.y + o.height, o.y - h, o.y, o.y + o.height - h]
                            for (var b = 0; b < candidatesY.length; ++b) {
                                if (Math.abs(y - candidatesY[b]) < snap) { y = candidatesY[b]; break }
                            }
                        }
                        DisplaySetup.setPosition(modelData.key, x, y)
                    }
                }
            }
        }
    }

    // ------------------------------------------------------- exact values
    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceSmall

        Label { text: qsTr("Display"); color: theme.textSecondary; font.pointSize: 9; Layout.fillWidth: true; Layout.minimumWidth: 110 }
        Label { text: qsTr("Width"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Height"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("X"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Y"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 76 }
        Label { text: qsTr("Workstation output"); color: theme.textSecondary; font.pointSize: 9; Layout.preferredWidth: 190 }
    }

    Repeater {
        model: DisplaySetup.monitors

        delegate: RowLayout {
            Layout.fillWidth: true
            spacing: theme.spaceSmall
            enabled: modelData.on

            Label {
                text: (modelData.primary ? "★ " : "") + modelData.name
                color: modelData.included ? theme.textPrimary : theme.textDisabled
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 110
            }
            PlankTextField {
                id: widthField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.width) : ""
                validator: IntValidator { bottom: 2; top: 16384 }
                onEditingFinished: {
                    if (parseInt(text) !== modelData.width &&
                            !DisplaySetup.setCustomSize(modelData.key, parseInt(text), parseInt(heightField.text))) {
                        text = String(modelData.width)
                    }
                }
            }
            PlankTextField {
                id: heightField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.height) : ""
                validator: IntValidator { bottom: 2; top: 16384 }
                onEditingFinished: {
                    if (parseInt(text) !== modelData.height &&
                            !DisplaySetup.setCustomSize(modelData.key, parseInt(widthField.text), parseInt(text))) {
                        text = String(modelData.height)
                    }
                }
            }
            PlankTextField {
                id: xField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.x) : ""
                validator: IntValidator { bottom: -16384; top: 16384 }
                onEditingFinished: {
                    if (parseInt(text) !== modelData.x) {
                        DisplaySetup.setPosition(modelData.key, parseInt(text), parseInt(yField.text))
                    }
                }
            }
            PlankTextField {
                id: yField
                Layout.preferredWidth: 76
                text: modelData.included ? String(modelData.y) : ""
                validator: IntValidator { bottom: -16384; top: 16384 }
                onEditingFinished: {
                    if (parseInt(text) !== modelData.y) {
                        DisplaySetup.setPosition(modelData.key, parseInt(xField.text), parseInt(text))
                    }
                }
            }
            PlankComboBox {
                Layout.preferredWidth: 190
                enabled: DisplaySetup.hostKind !== "legacy" && DisplaySetup.hostKind !== "mac"
                model: [qsTr("Automatic"), qsTr("Workstation screen"), qsTr("Virtual display")]
                currentIndex: page.backingIndex(modelData.preference)
                onActivated: function(index) {
                    DisplaySetup.setPreference(modelData.key, ["auto", "physical", "virtual"][index])
                }
            }
        }
    }

    // -------------------------------------------------------- presentation
    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceMedium

        Label {
            text: qsTr("On this computer")
            color: theme.textSecondary
        }
        PlankComboBox {
            Layout.fillWidth: true
            model: [qsTr("One fullscreen window per screen"), qsTr("One window for all screens")]
            currentIndex: DisplaySetup.presentation === "single" ? 1 : 0
            onActivated: function(index) { DisplaySetup.setPresentation(index === 1 ? "single" : "windows") }
        }
        PlankComboBox {
            Layout.fillWidth: true
            model: [qsTr("Native (1:1 pixels)"), qsTr("Scale to fit")]
            currentIndex: DisplaySetup.scaling === "fit" ? 1 : 0
            onActivated: function(index) { DisplaySetup.setScaling(index === 1 ? "fit" : "native") }
        }
    }

    Button {
        text: qsTr("Back to the suggested layout")
        flat: true
        onClicked: DisplaySetup.resetToProposal()
    }

    // ------------------------------------------------ older workstations
    ColumnLayout {
        id: fixed
        visible: page.hostId !== ""
        Layout.fillWidth: true
        Layout.topMargin: theme.spaceMedium
        spacing: 8

        property var setup: ({})
        property var modes: []

        function reload() {
            if (page.hostId === "") return
            setup = RemoteBroker.displaySetup(page.hostId)
            modes = setup.virtualModes || []
            fixedLayout.currentIndex = Math.max(0, setup.layoutChoice)
            fixedMode1.currentIndex = Math.max(0, modes.indexOf(setup.virtualMode1))
            fixedMode2.currentIndex = Math.max(0, modes.indexOf(setup.virtualMode2))
            fixedScaling.currentIndex = setup.scalingChoice
        }

        Component.onCompleted: reload()
        Connections {
            target: page
            function onHostIdChanged() { fixed.reload() }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: theme.borderSubtle
        }
        Label {
            text: qsTr("Older workstations and remote Macs")
            color: theme.textPrimary
            font.weight: Font.DemiBold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("A fixed layout for %1 from the sizes older PLANK versions know. It replaces the display setup for this workstation until you save a display setup for it again.").arg(page.hostName)
            color: theme.textSecondary
            wrapMode: Text.Wrap
            font.pointSize: 10
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: theme.spaceSmall

            PlankComboBox {
                id: fixedLayout
                Layout.fillWidth: true
                model: [qsTr("Match my display(s)"), qsTr("Workstation's own screens"),
                        qsTr("One virtual display"), qsTr("Two virtual displays side by side")]
            }
            PlankComboBox {
                id: fixedMode1
                Layout.preferredWidth: 140
                enabled: fixedLayout.currentIndex >= 2 || fixedLayout.currentIndex === 0
                model: fixed.modes.map(function(mode) { return mode.replace("x", " × ") })
            }
            PlankComboBox {
                id: fixedMode2
                visible: fixedLayout.currentIndex === 3
                Layout.preferredWidth: 140
                model: fixed.modes.map(function(mode) { return mode.replace("x", " × ") })
            }
            PlankComboBox {
                id: fixedScaling
                Layout.preferredWidth: 170
                model: [qsTr("Native (1:1 pixels)"), qsTr("Scale to fit")]
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: theme.spaceSmall

            Button {
                text: qsTr("Use this fixed layout")
                enabled: fixed.modes.length > 0
                onClicked: {
                    if (DisplaySetup.useFixedLayout(page.hostId, fixedLayout.currentIndex,
                                                    fixed.modes[fixedMode1.currentIndex],
                                                    fixed.modes[fixedMode2.currentIndex],
                                                    fixedScaling.currentIndex)) {
                        page.fixedLayoutSaved()
                    }
                }
            }
            Button {
                text: qsTr("Use the workstation's own screens as they are")
                flat: true
                enabled: fixed.modes.length > 0
                onClicked: {
                    if (DisplaySetup.useFixedLayout(page.hostId, 1, fixed.modes[fixedMode1.currentIndex],
                                                    fixed.modes[fixedMode2.currentIndex], 1)) {
                        page.fixedLayoutSaved()
                    }
                }
            }
        }
    }
}

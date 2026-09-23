import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3

import DisplaySetup 1.0

// The display setup at a glance: this computer's monitors on the left, the
// workstation desktop they become on the right, then one row per monitor
// (on/off, primary, size) and the planner's warnings with their actions.
// Everything comes from the DisplaySetup singleton (DisplayPlanner); this
// view only shows it and forwards choices.
ColumnLayout {
    id: panel

    // Smaller pictures, for the onboarding step and Settings.
    property bool compact: false
    // Read-only summary (Settings): no rows or actions.
    property bool readOnly: false

    spacing: theme.spaceMedium

    PlankTheme {
        id: theme
    }

    function backingColor(backing) {
        if (backing === "physical") return theme.success
        if (backing === "physical-viewport") return theme.warning
        if (backing === "virtual") return theme.accent
        return theme.textDisabled
    }

    function badgeColor(badge) {
        if (badge === "scaled" || badge === "closest") return theme.warning
        if (badge === "exact") return theme.success
        return theme.textSecondary
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceMedium

        // ------------------------------------------------------ your screens
        Rectangle {
            id: clientPane
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.preferredHeight: panel.compact ? 150 : 200
            color: theme.canvas
            radius: theme.radiusMedium
            border.width: 1
            border.color: theme.borderSubtle

            Label {
                id: clientTitle
                x: 12
                y: 8
                text: Qt.platform.os === "osx" ? qsTr("Your Mac") : qsTr("This computer")
                color: theme.textSecondary
                font.pointSize: 10
                font.weight: Font.DemiBold
            }

            Item {
                id: clientArea
                anchors.fill: parent
                anchors.margins: 14
                anchors.topMargin: 30

                readonly property var extent: DisplaySetup.clientExtent
                readonly property real scale: Math.min(width / Math.max(1, extent.width),
                                                       height / Math.max(1, extent.height))
                readonly property real offsetX: (width - extent.width * scale) / 2
                readonly property real offsetY: (height - extent.height * scale) / 2

                Repeater {
                    model: DisplaySetup.monitors

                    delegate: Rectangle {
                        x: clientArea.offsetX + (modelData.clientX - clientArea.extent.x) * clientArea.scale + 1
                        y: clientArea.offsetY + (modelData.clientY - clientArea.extent.y) * clientArea.scale + 1
                        width: Math.max(8, modelData.clientWidth * clientArea.scale - 2)
                        height: Math.max(8, modelData.clientHeight * clientArea.scale - 2)
                        radius: theme.radiusSmall
                        color: modelData.on ? theme.surfaceRaised : theme.canvas
                        border.width: modelData.primary ? 2 : 1
                        border.color: modelData.primary ? theme.accent : theme.border
                        opacity: modelData.on ? 1.0 : 0.55

                        // The camera housing of a notched MacBook.
                        Rectangle {
                            visible: modelData.notch
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: 1
                            width: parent.width * 0.16
                            height: Math.max(3, parent.height * 0.05)
                            radius: 2
                            color: theme.border
                        }

                        Column {
                            anchors.centerIn: parent
                            width: parent.width - 8
                            spacing: 1

                            Label {
                                width: parent.width
                                text: (modelData.primary ? "★ " : "") + modelData.name
                                color: modelData.on ? theme.textPrimary : theme.textDisabled
                                font.pointSize: 9
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                            Label {
                                width: parent.width
                                text: modelData.on ? modelData.exactText : qsTr("stays local")
                                color: theme.textSecondary
                                font.pointSize: 8
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !panel.readOnly
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: DisplaySetup.setMonitorOn(modelData.key, !modelData.on)
                        }

                        ToolTip.visible: hoverArea.containsMouse && !panel.readOnly
                        ToolTip.delay: 600
                        ToolTip.text: modelData.on ? qsTr("Click to keep this screen local") :
                                                     qsTr("Click to show the workstation on this screen")

                        MouseArea {
                            id: hoverArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                    }
                }
            }
        }

        Label {
            text: "→"
            color: theme.textSecondary
            font.pointSize: 16
            Layout.alignment: Qt.AlignVCenter
        }

        // ------------------------------------------------ workstation desktop
        Rectangle {
            id: hostPane
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.preferredHeight: clientPane.height
            color: theme.canvas
            radius: theme.radiusMedium
            border.width: 1
            border.color: theme.borderSubtle

            Label {
                x: 12
                y: 8
                width: parent.width - 24
                text: !DisplaySetup.planOk || DisplaySetup.desktop.width <= 0 ? qsTr("Workstation") :
                      DisplaySetup.desktop.packed ?
                          qsTr("Workstation · %1 × %2 · one %3 × %4 stream").arg(DisplaySetup.desktop.width)
                              .arg(DisplaySetup.desktop.height).arg(DisplaySetup.desktop.captureWidth)
                              .arg(DisplaySetup.desktop.captureHeight) :
                          qsTr("Workstation · %1 × %2").arg(DisplaySetup.desktop.width).arg(DisplaySetup.desktop.height)
                color: theme.textSecondary
                font.pointSize: 10
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Item {
                id: hostArea
                anchors.fill: parent
                anchors.margins: 14
                anchors.topMargin: 30

                readonly property var desk: DisplaySetup.desktop
                readonly property real scale: Math.min(width / Math.max(1, desk.width),
                                                       height / Math.max(1, desk.height))
                readonly property real offsetX: (width - desk.width * scale) / 2
                readonly property real offsetY: (height - desk.height * scale) / 2

                Repeater {
                    model: DisplaySetup.planOk ? DisplaySetup.monitors : []

                    delegate: Rectangle {
                        visible: modelData.included
                        x: hostArea.offsetX + modelData.x * hostArea.scale + 1
                        y: hostArea.offsetY + modelData.y * hostArea.scale + 1
                        width: Math.max(8, modelData.width * hostArea.scale - 2)
                        height: Math.max(8, modelData.height * hostArea.scale - 2)
                        radius: theme.radiusSmall
                        color: Qt.rgba(panel.backingColor(modelData.backing).r,
                                       panel.backingColor(modelData.backing).g,
                                       panel.backingColor(modelData.backing).b, 0.22)
                        border.width: modelData.primary ? 2 : 1
                        border.color: panel.backingColor(modelData.backing)

                        Column {
                            anchors.centerIn: parent
                            width: parent.width - 8
                            spacing: 1

                            Label {
                                width: parent.width
                                text: (modelData.primary ? "★ " : "") + modelData.sizeText
                                color: theme.textPrimary
                                font.pointSize: 9
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                            Label {
                                width: parent.width
                                text: modelData.backingText
                                color: theme.textSecondary
                                font.pointSize: 8
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Label {
                    anchors.fill: parent
                    visible: !DisplaySetup.planOk
                    text: DisplaySetup.planError
                    color: theme.warning
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // Legend: what backs each workstation display.
    RowLayout {
        Layout.fillWidth: true
        spacing: theme.spaceLarge
        visible: DisplaySetup.hostKind !== "legacy" && DisplaySetup.hostKind !== "mac"

        Repeater {
            model: [
                {"backing": "physical", "text": qsTr("Workstation screen (its own output)")},
                {"backing": "virtual", "text": qsTr("Virtual display, exact size")},
                {"backing": "physical-viewport", "text": qsTr("Workstation screen, scaled")}
            ]
            delegate: RowLayout {
                spacing: 6
                Rectangle {
                    width: 10
                    height: 10
                    radius: 2
                    color: panel.backingColor(modelData.backing)
                }
                Label {
                    text: modelData.text
                    color: theme.textSecondary
                    font.pointSize: 9
                }
            }
        }
        Item {
            Layout.fillWidth: true
        }
        Label {
            visible: DisplaySetup.backingExpected
            text: qsTr("Expected; the workstation decides when you connect")
            color: theme.textDisabled
            font.pointSize: 9
            font.italic: true
        }
    }

    Label {
        Layout.fillWidth: true
        visible: DisplaySetup.hostKind === "legacy"
        text: qsTr("This workstation's BDE fernweh only knows one screen or two side by side, from a fixed list of sizes.")
        color: theme.textSecondary
        wrapMode: Text.Wrap
        font.pointSize: 10
    }

    // ------------------------------------------------------ per-monitor rows
    Repeater {
        model: panel.readOnly ? [] : DisplaySetup.monitors

        delegate: ColumnLayout {
            id: row
            Layout.fillWidth: true
            spacing: 4

            property bool editingCustom: false

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceSmall

                PlankCheckBox {
                    checked: modelData.on
                    text: ""
                    Layout.preferredWidth: 28
                    onToggled: DisplaySetup.setMonitorOn(modelData.key, checked)
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Show the workstation on this screen")
                }

                ToolButton {
                    text: modelData.primary ? "★" : "☆"
                    enabled: modelData.on && !modelData.primary
                    font.pointSize: 13
                    Layout.preferredWidth: 32
                    onClicked: DisplaySetup.setPrimary(modelData.key)
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: modelData.primary ? qsTr("Primary screen (menu bar and new windows)") :
                                                      qsTr("Make this the primary screen")
                }

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    Layout.minimumWidth: 120

                    Label {
                        text: modelData.name
                        color: modelData.on ? theme.textPrimary : theme.textDisabled
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: modelData.on ? (modelData.included ? modelData.backingText : qsTr("Not shown (too many screens)")) :
                                             qsTr("Stays local")
                        color: theme.textSecondary
                        font.pointSize: 9
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                PlankComboBox {
                    id: sizeBox
                    enabled: modelData.on && DisplaySetup.hostKind !== "legacy" && DisplaySetup.hostKind !== "mac"
                    Layout.preferredWidth: 250
                    model: modelData.sizeOptions.map(function(option) { return option.label })
                    currentIndex: modelData.sizeIndex
                    onActivated: function(index) {
                        var option = modelData.sizeOptions[index]
                        if (option.value === "custom") {
                            row.editingCustom = true
                            customWidth.text = String(modelData.width || 1920)
                            customHeight.text = String(modelData.height || 1080)
                            customWidth.forceActiveFocus()
                        } else {
                            row.editingCustom = false
                            DisplaySetup.setSize(modelData.key, option.value)
                        }
                    }
                }

                Label {
                    text: modelData.badgeText
                    visible: modelData.included && text !== ""
                    color: panel.badgeColor(modelData.badge)
                    font.pointSize: 9
                    Layout.preferredWidth: 90
                }
            }

            RowLayout {
                visible: row.editingCustom
                Layout.leftMargin: 66
                spacing: theme.spaceSmall

                PlankTextField {
                    id: customWidth
                    Layout.preferredWidth: 90
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                }
                Label {
                    text: "×"
                    color: theme.textSecondary
                }
                PlankTextField {
                    id: customHeight
                    Layout.preferredWidth: 90
                    validator: IntValidator { bottom: 2; top: 16384 }
                    inputMethodHints: Qt.ImhDigitsOnly
                    Keys.onReturnPressed: customApply.clicked()
                    Keys.onEnterPressed: customApply.clicked()
                }
                Button {
                    id: customApply
                    text: qsTr("Set size")
                    onClicked: {
                        if (DisplaySetup.setCustomSize(modelData.key, parseInt(customWidth.text), parseInt(customHeight.text))) {
                            row.editingCustom = false
                            customError.text = ""
                        } else {
                            customError.text = qsTr("Use an even size the workstation supports.")
                        }
                    }
                }
                Button {
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: {
                        row.editingCustom = false
                        customError.text = ""
                    }
                }
                Label {
                    id: customError
                    color: theme.warning
                    font.pointSize: 9
                }
            }
        }
    }

    // --------------------------------------------------------------- warnings
    Repeater {
        model: panel.readOnly ? [] : DisplaySetup.warnings

        delegate: Rectangle {
            // Info (a packed capture) is news in the accent colour, not a warning.
            readonly property color tone: modelData.info ? theme.accent : theme.warning
            Layout.fillWidth: true
            implicitHeight: warningRow.implicitHeight + 16
            radius: theme.radiusSmall
            color: Qt.rgba(tone.r, tone.g, tone.b, 0.12)
            border.width: 1
            border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.45)

            RowLayout {
                id: warningRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: theme.spaceMedium

                Label {
                    text: modelData.text
                    color: theme.textPrimary
                    wrapMode: Text.Wrap
                    font.pointSize: 10
                    Layout.fillWidth: true
                }
                Button {
                    visible: modelData.action !== ""
                    text: modelData.actionLabel
                    onClicked: DisplaySetup.runAction(modelData.action, modelData.key)
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: DisplaySetup.summary
        color: DisplaySetup.planOk ? theme.textSecondary : theme.warning
        wrapMode: Text.Wrap
        font.pointSize: 10
    }
}

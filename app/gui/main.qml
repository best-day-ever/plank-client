import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

ApplicationWindow {
    property bool pollingActive: false

    id: window
    title: qsTr("BDE Fernweh Client")
    width: 1280
    height: 1200
    minimumHeight: 900

    PlankTheme {
        id: theme
    }

    Material.theme: Material.Dark
    Material.background: theme.canvas
    Material.foreground: theme.textPrimary
    Material.primary: theme.chrome
    Material.accent: theme.accent

    Component.onCompleted: {
        // The PLANK launcher is always a normal desktop window.
        window.show()

        // Display any modal dialogs for configuration warnings
        if (SystemProperties.isWow64) {
            wow64Dialog.open()
        }
        else if (!SystemProperties.hasHardwareAcceleration) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }

    }
  
    // ToolTip is an attached property and must be hosted on an Item rather
    // than directly on ApplicationWindow under Qt 6.10.
    Item {
        visible: false
        width: 0
        height: 0

        Text {
            id: tooltipTextLayoutHelper
            visible: false
            font: ToolTip.toolTip.font
            text: ToolTip.toolTip.text
        }

        ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)
    }

    function goBack() {
        stackView.pop()
    }

    StackView {
        id: stackView
        initialItem: initialView
        anchors.fill: parent
        focus: true

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }
    }

    // Workaround for lack of instanceof in Qt 5.9.
    //
    // Based on https://stackoverflow.com/questions/13923794/how-to-do-a-is-a-typeof-or-instanceof-in-qml
    function qmltypeof(obj, className) { // QtObject, string -> bool
        if (obj === null || obj === undefined) {
            return false
        }
        // className plus "(" is the class instance without modification
        // className plus "_QML" is the class instance with user-defined properties
        var str = obj.toString();
        return str.startsWith(className + "(") || str.startsWith(className + "_QML");
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return qmltypeof(item, objectType)
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    header: ToolBar {
        id: toolBar
        height: 56

        background: Rectangle {
            color: theme.chrome

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: theme.borderSubtle
            }
        }

        Label {
            id: titleLabel
            visible: toolBar.width > 700
            anchors.fill: parent
            text: stackView.currentItem ? stackView.currentItem.objectName : ""
            color: theme.textPrimary
            font.pointSize: 15
            font.weight: Font.DemiBold
            elide: Label.ElideRight
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
        }

        RowLayout {
            spacing: theme.spaceSmall
            anchors.leftMargin: theme.spaceMedium
            anchors.rightMargin: theme.spaceMedium
            anchors.fill: parent

            NavigableToolButton {
                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1

                iconSource: "qrc:/res/arrow_left.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            Label {
                id: plankVersionLabel
                text: SystemProperties.plankVersionString
                color: theme.textSecondary
                font.pointSize: 9
                font.weight: Font.Medium
                horizontalAlignment: Qt.AlignLeft
                verticalAlignment: Qt.AlignVCenter
            }

            // This label will appear when the window gets too small and
            // we need to ensure the toolbar controls don't collide
            Label {
                id: titleRowLabel
                font.pointSize: titleLabel.font.pointSize
                elide: Label.ElideRight
                horizontalAlignment: Qt.AlignHCenter
                verticalAlignment: Qt.AlignVCenter
                Layout.fillWidth: true

                // We need this label to always be visible so it can occupy
                // the remaining space in the RowLayout. To "hide" it, we
                // just set the text to empty string.
                text: !titleLabel.visible && stackView.currentItem ? stackView.currentItem.objectName : ""
            }

            NavigableToolButton {
                id: localButton
                visible: qmltypeof(stackView.currentItem, "RemoteView")

                iconSource: "qrc:/res/workstations.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Local workstations")

                onClicked: navigateTo("qrc:/gui/PcView.qml", "PcView")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: remoteButton
                visible: qmltypeof(stackView.currentItem, "PcView")

                iconSource: "qrc:/res/remote.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Remote access")

                onClicked: navigateTo("qrc:/gui/RemoteView.qml", "RemoteView")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: addPcButton
                visible: qmltypeof(stackView.currentItem, "PcView")

                iconSource:  "qrc:/res/ic_add_to_queue_white_48px.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: settingsButton

                iconSource:  "qrc:/res/settings.svg"

                onClicked: navigateTo("qrc:/gui/SettingsView.qml", "SettingsView")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by PLANK. " +
                   "Your streaming performance may be severely degraded in this configuration.")
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok
        text: qsTr("This PLANK build isn't optimized for your PC. Please install the '%1' PLANK package for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
    }

    // This dialog appears when quitting via keyboard.
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Are you sure you want to quit?")
        // For keyboard navigation
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        title: qsTr("Add workstation bookmark")
        property var virtualModeChoices: ComputerManager.plankVirtualModeChoices()

        // Give both connection fields enough room for real hostnames while
        // keeping the dialog inside smaller launcher windows. The dialog still
        // blocks the launcher, but it must not wash out the UI behind it.
        width: Math.min(640, parent.width - 40)
        height: Math.min(implicitHeight, parent.height - 20)
        dim: false

        function suggestedNickname(address) {
            var value = address.trim()
            if (value === "" || value.charAt(0) === "[" || /^\d{1,3}(\.\d{1,3}){3}(:\d+)?$/.test(value)) {
                return ""
            }
            return value.split(":")[0].split(".")[0]
        }

        function currentVideoProfile() {
            return addVideoSettings.videoProfile
        }

        function virtualModeSupported(mode) {
            return StreamingPreferences.plankVirtualModeSupportedForProfile(
                        mode, currentVideoProfile())
        }

        function ensureVirtualModesCompatible() {
            if (addVideoSettings.captureSource === 2) return
            var fallback = -1
            for (var i = 0; i < virtualModeChoices.length; ++i) {
                if (virtualModeChoices[i] === "4096\u00d72160") {
                    fallback = i
                    break
                }
            }
            if (fallback < 0) {
                return
            }
            if (!virtualModeSupported(virtualModeChoices[addVirtualMode1.currentIndex])) {
                addVirtualMode1.currentIndex = fallback
            }
            if (!virtualModeSupported(virtualModeChoices[addVirtualMode2.currentIndex])) {
                addVirtualMode2.currentIndex = fallback
            }
        }

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            addressText.forceActiveFocus()
            addVideoSettings.load(StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT,
                                  StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                                  StreamingPreferences.plankDefaultProfileBitratesKbps(), [])
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return addressText.text.trim() !== "" && nicknameText.text.trim() !== ""
            })
        }

        onClosed: {
            addressText.clear()
            nicknameText.clear()
            nicknameText.manuallyEdited = false
            addHostLayout.currentIndex = 0
            addVirtualMode1.currentIndex = 9
            addVirtualMode2.currentIndex = 1
            addScalingChoice.currentIndex = 1
        }

        onAccepted: {
            if (addressText.text && nicknameText.text) {
                ComputerManager.addNewHostManually(addressText.text.trim(),
                                                   nicknameText.text.trim(),
                                                   addHostLayout.currentIndex,
                                                   addVirtualMode1.currentIndex,
                                                   addVirtualMode2.currentIndex,
                                                   addScalingChoice.currentIndex,
                                                   addVideoSettings.videoProfile,
                                                   addVideoSettings.captureSource,
                                                   addVideoSettings.officeBitratesKbps)
            }
        }

        ColumnLayout {
            width: parent.width

            Label {
                text: qsTr("Address or hostname")
                font.bold: true
            }

            PlankTextField {
                id: addressText
                Layout.fillWidth: true
                focus: true

                onTextEdited: {
                    if (!nicknameText.manuallyEdited) {
                        nicknameText.text = addPcDialog.suggestedNickname(text)
                    }
                }

                Keys.onReturnPressed: nicknameText.forceActiveFocus()
                Keys.onEnterPressed: nicknameText.forceActiveFocus()
            }

            Label {
                text: qsTr("Nickname")
                font.bold: true
            }

            PlankTextField {
                id: nicknameText
                property bool manuallyEdited: false

                Layout.fillWidth: true

                onTextEdited: manuallyEdited = true

                Keys.onReturnPressed: {
                    if (addressText.text && text) {
                        addPcDialog.accept()
                    }
                }

                Keys.onEnterPressed: {
                    if (addressText.text && text) {
                        addPcDialog.accept()
                    }
                }
            }

            PlankVideoSettings {
                id: addVideoSettings
                Layout.fillWidth: true
                hostAddress: addressText.text
                probingEnabled: addPcDialog.visible
                nvfbcDefaultProfile: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
                onCaptureSourceEdited: addHostLayout.currentIndex = 0
                onProfileActivated: addPcDialog.ensureVirtualModesCompatible()
            }

            Label {
                text: qsTr("Host display layout")
                font.bold: true
            }

            PlankComboBox {
                id: addHostLayout
                Layout.fillWidth: true
                model: addVideoSettings.captureSource === 2 ? [qsTr("Match client display(s)"), qsTr("One Mac virtual display")] : [
                    qsTr("Match client displays"),
                    qsTr("Physical displays"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                text: addVideoSettings.captureSource === 2 ? qsTr("Mac desktop resolution") : qsTr("Virtual display 1 resolution")
                font.bold: true
                opacity: (addVideoSettings.captureSource === 2 ? addHostLayout.currentIndex === 1 : addHostLayout.currentIndex >= 2) ? 1.0 : 0.5
            }

            PlankComboBox {
                id: addVirtualMode1
                Layout.fillWidth: true
                enabled: (addVideoSettings.captureSource === 2 ? addHostLayout.currentIndex === 1 : addHostLayout.currentIndex >= 2)
                currentIndex: 9
                model: addPcDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: addVirtualMode1.width
                    text: modelData
                    enabled: addPcDialog.virtualModeSupported(modelData)
                    highlighted: addVirtualMode1.highlightedIndex === index
                }
            }

            Label {
                visible: addVideoSettings.captureSource !== 2
                text: qsTr("Virtual display 2 resolution")
                font.bold: true
                opacity: addHostLayout.currentIndex === 3 ? 1.0 : 0.5
            }

            PlankComboBox {
                id: addVirtualMode2
                visible: addVideoSettings.captureSource !== 2
                Layout.fillWidth: true
                enabled: addHostLayout.currentIndex === 3
                currentIndex: 1
                model: addPcDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: addVirtualMode2.width
                    text: modelData
                    enabled: addPcDialog.virtualModeSupported(modelData)
                    highlighted: addVirtualMode2.highlightedIndex === index
                }
            }

            Label {
                text: qsTr("Scaling")
                font.bold: true
            }

            PlankComboBox {
                id: addScalingChoice
                Layout.fillWidth: true
                currentIndex: 1
                model: [qsTr("Native (1:1 pixels)"), qsTr("Scaled-Span")]
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Native preserves one host pixel per streamed pixel. Scaled-Span fits the complete host desktop into the client resolution.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }
        }
    }
}

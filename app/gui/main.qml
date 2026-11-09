import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import DisplaySetup 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import FernwehUpdater 1.0
import RemoteBroker 1.0

ApplicationWindow {
    property alias pollingActive: hostPolling.pollingActive
    PlankHostPolling {
        id: hostPolling
        manager: ComputerManager
        windowVisible: window.visible
        windowActive: window.active
    }

    function openNewConnection() {
        if (!SystemProperties.openNewConnection()) newConnectionError.open()
    }

    NavigableMessageDialog {
        id: newConnectionError
        title: qsTr("New connection")
        text: qsTr("Could not open another Fernweh window. Wait for any application update to finish, then try again.")
        standardButtons: Dialog.Ok
    }

    Shortcut {
        sequence: StandardKey.New
        onActivated: window.openNewConnection()
    }

    Timer {
        interval: 6 * 60 * 60 * 1000
        running: FernwehUpdater.supported
        repeat: true
        onTriggered: FernwehUpdater.check()
    }

    id: window
    title: Qt.application.displayName
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
        FernwehUpdater.check()

        // A brand-new installation starts with the first sign-in wizard on
        // top of the remote sign-in (main.cpp / OnboardingController decide).
        if (typeof showOnboarding !== "undefined" && showOnboarding) {
            stackView.push("qrc:/gui/OnboardingView.qml")
        }

        // Display any modal dialogs for configuration warnings
        if (SystemProperties.isWow64) {
            wow64Dialog.open()
        }
        else if (SystemProperties.hardwareCapabilityKnown && !SystemProperties.hasHardwareAcceleration) {
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

    // The display setup wizard: from the first connect with new screens, a
    // workstation's Settings…, Settings > Displays and the banner below.
    DisplaySetupDialog {
        id: screensDialog
    }

    function openDisplaySetup(hostId, hostName, reason, connectAfter, page) {
        screensDialog.openFor(hostId, hostName, reason, connectAfter, page)
    }

    // Screens changed (dock, undock, a lid opened) while no stream runs.
    BrokerSessionBanner {
        z: 20
        message: RemoteBroker.sessionNotice
        width: Math.min(760, parent.width - 40)
        height: implicitHeight
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 12
        onDismissed: RemoteBroker.dismissSessionNotice()
    }

    Rectangle {
        id: displayBanner
        readonly property bool streaming: qmltypeof(stackView.currentItem, "StreamSegue")
        readonly property bool onboarding: qmltypeof(stackView.currentItem, "OnboardingView")
        visible: DisplaySetup.bannerKind !== "" && !streaming && !onboarding && !screensDialog.visible
        z: 10
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: Math.min(760, parent.width - 40)
        height: bannerRow.implicitHeight + 24
        radius: theme.radiusLarge
        color: theme.surfaceRaised
        border.width: 1
        border.color: DisplaySetup.bannerKind === "saved" ? theme.borderSubtle : theme.accent

        // A saved layout is only news for a moment.
        Timer {
            running: displayBanner.visible && DisplaySetup.bannerKind === "saved"
            interval: 8000
            onTriggered: DisplaySetup.dismissBanner()
        }

        RowLayout {
            id: bannerRow
            anchors.fill: parent
            anchors.margins: 12
            spacing: theme.spaceMedium

            Label {
                text: DisplaySetup.bannerText
                color: theme.textPrimary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Button {
                text: DisplaySetup.bannerKind === "saved" ? qsTr("Change…") :
                      DisplaySetup.bannerKind === "choose" ? qsTr("Choose screens…") : qsTr("Set up…")
                highlighted: DisplaySetup.bannerKind !== "saved"
                onClicked: {
                    var reason = DisplaySetup.bannerKind === "new" ? "new-screens" : ""
                    DisplaySetup.dismissBanner()
                    openDisplaySetup("", "", reason, false, 0)
                }
            }
            Button {
                text: DisplaySetup.bannerKind === "saved" ? qsTr("OK") : qsTr("Later")
                flat: true
                onClicked: DisplaySetup.dismissBanner()
            }
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
            // Center only when neither side's controls would overlap the title.
            visible: toolBar.width > 2 * Math.max(titleRowLabel.x,
                         toolBar.width - titleRowLabel.x - titleRowLabel.width) + implicitWidth + 24
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

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Back")

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            Button {
                text: qsTr("New connection")
                onClicked: window.openNewConnection()
            }

            Label {
                id: plankVersionLabel
                text: qsTr("BDE fernweh %1").arg(SystemProperties.plankVersionString)
                color: theme.textSecondary
                font.pointSize: 9
                font.weight: Font.Medium
                horizontalAlignment: Qt.AlignLeft
                verticalAlignment: Qt.AlignVCenter
            }

            Button {
                visible: FernwehUpdater.available
                enabled: !FernwehUpdater.busy
                text: qsTr("Update to %1").arg(FernwehUpdater.version)
                onClicked: FernwehUpdater.install()
            }

            Label {
                visible: FernwehUpdater.status !== ""
                text: FernwehUpdater.status
                color: theme.textSecondary
                font.pointSize: 9
                Layout.maximumWidth: 280
                wrapMode: Text.WordWrap
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

            PlankInternetStatus {
                monitor: internetConnectivity
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
                ToolTip.text: qsTr("Add workstation") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

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
        text: qsTr("No functioning hardware accelerated video decoder was detected by BDE fernweh. " +
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
        text: qsTr("This BDE fernweh build isn't optimized for your PC. Please install the '%1' package for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
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
        // keeping the dialog inside smaller launcher windows. Modal, so a
        // stray click beside it cannot throw away what was typed.
        width: Math.min(640, parent.width - 40)
        height: Math.min(implicitHeight, parent.height - 20)
        modal: true
        closePolicy: Popup.CloseOnEscape

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
            standardButton(Dialog.Ok).text = qsTr("Add")
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
            addScalingChoice.currentIndex = 0
            addClientDisplays.selection = "current"
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
                                                   addVideoSettings.officeBitratesKbps,
                                                   addClientDisplays.selection)
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
                Layout.topMargin: 10
                text: qsTr("Host display layout")
                font.bold: true
            }

            PlankComboBox {
                id: addHostLayout
                Layout.fillWidth: true
                model: addVideoSettings.captureSource === 2 ? [qsTr("Match client display(s)"), qsTr("One Mac virtual display")] : [
                    qsTr("Match client displays"),
                    qsTr("Match Host"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                Layout.fillWidth: true
                visible: addVideoSettings.captureSource !== 2 && addHostLayout.currentIndex === 0
                text: visible ? ComputerManager.plankMatchClientSummary() : ""
                wrapMode: Text.Wrap
                opacity: 0.72
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

            PlankClientDisplayBox {
                id: addClientDisplays
                Layout.fillWidth: true
            }

            Label {
                Layout.topMargin: 10
                text: qsTr("Scaling")
                font.bold: true
            }

            PlankComboBox {
                id: addScalingChoice
                Layout.fillWidth: true
                currentIndex: 0
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

import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid

    StationConnectTheme {
        id: theme
    }

    focus: true
    activeFocusOnTab: true
    topMargin: 24
    bottomMargin: 24
    // A workstation is the primary object on this page, so let its row use
    // the complete content width instead of stopping at an arbitrary cap.
    cellWidth: availableWidth
    cellHeight: 88
    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function authenticationComplete(error)
    {
        var pcIndex = loginDialog.pcIndex
        loginDialog.close()
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.open()
        } else {
            launchStationConnectDesktop(pcIndex)
        }
    }

    function launchStationConnectDesktop(pcIndex)
    {
        var session = computerModel.createSessionForStationConnectDesktop(pcIndex)
        if (session === null) {
            errorDialog.text = qsTr("The workstation did not provide its Desktop session.")
            errorDialog.open()
            return
        }

        var component = Qt.createComponent("StreamSegue.qml")
        var segue = component.createObject(stackView, {
                                               "appName": qsTr("Desktop"),
                                               "session": session,
                                               "isResume": false
                                           })
        stackView.push(segue)
    }

    function addComplete(success)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.authenticationCompleted.connect(authenticationComplete)
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: theme.spaceMedium
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            color: theme.textSecondary
            font.pointSize: 15
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        id: pcEntry
        width: pcGrid.cellWidth
        height: 76
        grid: pcGrid
        Accessible.name: model.name
        hoverEnabled: true

        background: Rectangle {
            radius: theme.radiusMedium
            color: pcEntry.down ? theme.surfacePressed :
                   (pcEntry.hovered || pcEntry.highlighted ? theme.surfaceHover : theme.surface)
            border.width: pcEntry.highlighted ? 1 : 0
            border.color: theme.accent

            Behavior on color {
                ColorAnimation { duration: 90 }
            }
        }

        property alias pcContextMenu : pcContextMenuLoader.item

        Image {
            id: stateIcon
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            visible: !model.statusUnknown
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" :
                                    (!model.authorized ? "qrc:/res/baseline-lock-24px.svg" :
                                                     "qrc:/res/baseline-check_circle_outline-24px.svg")
            sourceSize {
                width: 28
                height: 28
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: stateIcon.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            visible: model.statusUnknown
        }

        Column {
            id: workstationIdentity
            anchors.left: stateIcon.right
            anchors.leftMargin: 14
            anchors.right: workstationStatus.left
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                id: pcNameText
                width: parent.width
                text: model.name
                color: theme.textPrimary
                font.pointSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                text: model.address ? model.address : qsTr("Address unavailable")
                color: theme.textSecondary
                font.pointSize: 10
                elide: Text.ElideRight
            }
        }

        Column {
            id: workstationStatus
            width: 190
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                width: parent.width
                text: model.statusUnknown ? qsTr("Checking") :
                      (model.online ? qsTr("Online") : qsTr("Offline"))
                color: model.statusUnknown ? theme.textSecondary :
                       (model.online ? theme.success : theme.textDisabled)
                font.pointSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
            }

            Label {
                width: parent.width
                text: model.stationConnectHostVersion ?
                          model.stationConnectHostVersion : " "
                color: theme.textSecondary
                font.pointSize: 9
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
            }
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Edit bookmark…")
                    visible: model.manualBookmark
                    onTriggered: {
                        editBookmarkDialog.pcIndex = index
                        editBookmarkDialog.originalAddress = model.address
                        editBookmarkDialog.originalNickname = model.name
                        editBookmarkDialog.scalingIndex =
                                computerModel.stationConnectScalingChoice(index)
                        editBookmarkDialog.hostLayoutIndex =
                                computerModel.stationConnectHostLayoutChoice(index)
                        editBookmarkDialog.virtualMode1Index =
                                computerModel.stationConnectVirtualMode1Choice(index)
                        editBookmarkDialog.virtualMode2Index =
                                computerModel.stationConnectVirtualMode2Choice(index)
                        editBookmarkDialog.originalProfile = computerModel.stationConnectVideoProfile(index)
                        editBookmarkDialog.originalCaptureSource =
                                computerModel.stationConnectCaptureSource(index)
                        editBookmarkDialog.originalProfileBitratesKbps =
                                computerModel.stationConnectProfileBitratesKbps(index)
                        editBookmarkDialog.open()
                    }
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                    visible: !model.manualBookmark
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (model.authorized) {
                    launchStationConnectDesktop(index)
                }
                else {
                    loginDialog.pcIndex = index
                    loginDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog
    }

    NavigableDialog {
        id: loginDialog
        property int pcIndex: -1
        title: qsTr("Sign in to workstation")
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: usernameField.forceActiveFocus()
        onClosed: {
            usernameField.clear()
            passwordField.clear()
        }
        onAccepted: {
            if (usernameField.text && passwordField.text) {
                computerModel.authenticateComputer(pcIndex, usernameField.text,
                                                   passwordField.text)
            }
        }

        ColumnLayout {
            Label {
                text: qsTr("Use your workstation operating-system account.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            StationConnectTextField {
                id: usernameField
                placeholderText: qsTr("Username")
                Layout.fillWidth: true
                focus: true
            }
            StationConnectTextField {
                id: passwordField
                placeholderText: qsTr("Password")
                echoMode: TextInput.Password
                Layout.fillWidth: true
                Keys.onReturnPressed: loginDialog.accept()
                Keys.onEnterPressed: loginDialog.accept()
            }
        }
    }

    NavigableDialog {
        id: editBookmarkDialog
        property int pcIndex: -1
        property string originalAddress: ""
        property string originalNickname: ""
        property int scalingIndex: 1
        property int hostLayoutIndex: 0
        property int hostDisplayPolicy: -1
        property int virtualMode1Index: 9
        property int virtualMode2Index: 1
        property var virtualModeChoices: ComputerManager.stationConnectVirtualModeChoices()
        property int originalProfile: StreamingPreferences.SCVP_H264_10BIT_444
        property int originalCaptureSource: StreamingPreferences.SCCS_NVFBC_8BIT
        property var originalProfileBitratesKbps: []
        property var profileBitratesKbps: []
        title: qsTr("Edit workstation bookmark")
        width: Math.min(640, parent.width - 40)
        height: Math.min(implicitHeight, parent.height - 20)
        dim: false
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        function currentVideoProfile() {
            if (editEncodingProfile.currentIndex >= 0) {
                return editEncodingProfile.model.get(
                            editEncodingProfile.currentIndex).val
            }
            return StreamingPreferences.SCVP_H264_10BIT_444
        }

        function applyProfileBitrate() {
            editBitrateSlider.value = profileBitratesKbps[currentVideoProfile()]
        }

        function rememberProfileBitrate() {
            var values = []
            for (var i = 0; i < profileBitratesKbps.length; ++i) {
                values.push(profileBitratesKbps[i])
            }
            values[currentVideoProfile()] = Math.round(editBitrateSlider.value)
            profileBitratesKbps = values
        }

        onOpened: {
            editAddressText.text = originalAddress
            editNicknameText.text = originalNickname
            editScalingChoice.currentIndex = scalingIndex
            hostDisplayPolicy = computerModel.stationConnectHostDisplayPolicy(pcIndex)
            editHostLayout.currentIndex = hostLayoutIndex
            editVirtualMode1.currentIndex = virtualMode1Index
            editVirtualMode2.currentIndex = virtualMode2Index
            editCaptureSource.currentIndex = originalCaptureSource
            for (var i = 0; i < editEncodingProfile.model.count; i++) {
                if (editEncodingProfile.model.get(i).val === originalProfile) {
                    editEncodingProfile.currentIndex = i
                    break
                }
            }
            var loadedBitrates = []
            for (var bitrateIndex = 0;
                 bitrateIndex < originalProfileBitratesKbps.length;
                 ++bitrateIndex) {
                loadedBitrates.push(originalProfileBitratesKbps[bitrateIndex])
            }
            profileBitratesKbps = loadedBitrates
            applyProfileBitrate()
            editAddressText.forceActiveFocus()
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return editAddressText.text.trim() !== "" &&
                       editNicknameText.text.trim() !== "" &&
                       editScalingChoice.currentIndex >= 0
            })
        }
        onClosed: {
            editAddressText.clear()
            editNicknameText.clear()
            hostDisplayPolicy = -1
            originalProfileBitratesKbps = []
            profileBitratesKbps = []
        }
        onAccepted: {
            if (!computerModel.editComputerBookmark(pcIndex,
                                                    editAddressText.text.trim(),
                                                    editNicknameText.text.trim(),
                                                    editScalingChoice.currentIndex,
                                                    editHostLayout.currentIndex,
                                                    editVirtualMode1.currentIndex,
                                                    editVirtualMode2.currentIndex,
                                                    editEncodingProfile.model.get(
                                                        editEncodingProfile.currentIndex).val,
                                                    editCaptureSourceModel.get(
                                                        editCaptureSource.currentIndex).val,
                                                    profileBitratesKbps)) {
                errorDialog.text = qsTr("Unable to update the workstation bookmark. Check the address and ensure another bookmark is not already using it.")
                errorDialog.open()
            }
        }

        ColumnLayout {
            width: parent.width

            Label {
                text: qsTr("Address or hostname")
                font.bold: true
            }
            StationConnectTextField {
                id: editAddressText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Nickname")
                font.bold: true
            }
            StationConnectTextField {
                id: editNicknameText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Capture source")
                font.bold: true
            }
            StationConnectComboBox {
                id: editCaptureSource
                Layout.fillWidth: true
                textRole: "text"
                model: ListModel {
                    id: editCaptureSourceModel
                    ListElement {
                        text: qsTr("NvFBC — 8-bit source")
                        val: StreamingPreferences.SCCS_NVFBC_8BIT
                    }
                    ListElement {
                        text: qsTr("Native X11/XShm — 10-bit (Experimental)")
                        val: StreamingPreferences.SCCS_X11_NATIVE10
                    }
                }
                onCurrentIndexChanged: {
                    if (currentIndex === 1) {
                        editEncodingProfile.currentIndex = 0
                    } else {
                        editEncodingProfile.currentIndex = 3
                    }
                    Qt.callLater(editBookmarkDialog.applyProfileBitrate)
                }
            }

            Label {
                text: qsTr("Encoding profile")
                font.bold: true
                opacity: editCaptureSource.currentIndex === 0 ? 1.0 : 0.5
            }
            StationConnectComboBox {
                id: editEncodingProfile
                Layout.fillWidth: true
                textRole: "text"
                model: editCaptureSource.currentIndex === 0 ?
                           editNvfbcEncodingProfileModel : editNativeEncodingProfileModel
                onActivated: editBookmarkDialog.applyProfileBitrate()
            }

            ListModel {
                id: editNvfbcEncodingProfileModel
                ListElement {
                    text: qsTr("H.264 8-bit 4:2:2")
                    val: StreamingPreferences.SCVP_H264_8BIT_422
                }
                ListElement {
                    text: qsTr("H.264 8-bit 4:4:4 (identity GBR)")
                    val: StreamingPreferences.SCVP_H264_8BIT_444
                }
                ListElement {
                    text: qsTr("H.264 10-bit 4:2:2")
                    val: StreamingPreferences.SCVP_H264_10BIT_422
                }
                ListElement {
                    text: qsTr("H.264 10-bit 4:4:4 (identity GBR)")
                    val: StreamingPreferences.SCVP_H264_10BIT_444
                }
                ListElement {
                    text: qsTr("H.264 8-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.SCVP_NVENC_H264_8BIT_444
                }
                ListElement {
                    text: qsTr("H.265 8-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.SCVP_NVENC_HEVC_8BIT_444
                }
                ListElement {
                    text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.SCVP_NVENC_HEVC_10BIT_444
                }
            }

            ListModel {
                id: editNativeEncodingProfileModel
                ListElement {
                    text: qsTr("H.264 10-bit 4:4:4 (identity GBR) — x264")
                    val: StreamingPreferences.SCVP_H264_10BIT_444
                }
                ListElement {
                    text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.SCVP_NVENC_HEVC_10BIT_444
                }
            }

            Label {
                text: qsTr("Startup encoder target: %1 Mbps").arg(
                          (editBitrateSlider.value / 1000.0).toFixed(1))
                font.bold: true
            }

            Slider {
                id: editBitrateSlider
                Layout.fillWidth: true
                from: StreamingPreferences.stationConnectBitrateMinimumKbps()
                to: StreamingPreferences.stationConnectBitrateMaximumKbps()
                stepSize: StreamingPreferences.stationConnectBitrateStepKbps()
                snapMode: Slider.SnapAlways
                onMoved: editBookmarkDialog.rememberProfileBitrate()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Saved independently for each encoding profile. Toolbar adjustments apply only to the active session.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }

            Label {
                text: qsTr("Host display layout")
                font.bold: true
            }
            StationConnectComboBox {
                id: editHostLayout
                Layout.fillWidth: true
                model: [
                    qsTr("Match client displays"),
                    qsTr("Physical displays"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                Layout.fillWidth: true
                visible: editBookmarkDialog.hostDisplayPolicy === 0
                text: qsTr("This headless workstation does not provide physical displays.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }

            Label {
                text: qsTr("Virtual display 1 resolution")
                font.bold: true
                opacity: editHostLayout.currentIndex >= 2 ? 1.0 : 0.5
            }
            StationConnectComboBox {
                id: editVirtualMode1
                Layout.fillWidth: true
                enabled: editHostLayout.currentIndex >= 2
                model: editBookmarkDialog.virtualModeChoices
            }

            Label {
                text: qsTr("Virtual display 2 resolution")
                font.bold: true
                opacity: editHostLayout.currentIndex === 3 ? 1.0 : 0.5
            }
            StationConnectComboBox {
                id: editVirtualMode2
                Layout.fillWidth: true
                enabled: editHostLayout.currentIndex === 3
                model: editBookmarkDialog.virtualModeChoices
            }

            Label {
                text: qsTr("Scaling")
                font.bold: true
            }
            StationConnectComboBox {
                id: editScalingChoice
                Layout.fillWidth: true
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

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            StationConnectTextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    ScrollBar.vertical: ScrollBar {}
}

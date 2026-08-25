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
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: Math.min(760, availableWidth)
    cellHeight: 104
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
            errorDialog.helpText = ""
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
            errorDialog.helpText = ""
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

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.authenticationCompleted.connect(authenticationComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
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
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: pcGrid.cellWidth - 10
        height: 94
        grid: pcGrid
        Accessible.name: model.name

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
                width: 44
                height: 44
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: stateIcon.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 44
            height: 44
            visible: model.statusUnknown
        }

        Column {
            anchors.left: stateIcon.right
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                id: pcNameText
                width: parent.width
                text: model.name
                font.pointSize: 24
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                text: (model.address ? model.address + "  ·  " : "") +
                      (model.statusUnknown ? qsTr("Checking") :
                       (model.online ? qsTr("Online") : qsTr("Offline")))
                font.pointSize: 12
                opacity: 0.72
                elide: Text.ElideRight
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
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: false
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Edit bookmark…")
                    visible: model.manualBookmark
                    onTriggered: {
                        var choices = computerModel.stationConnectDisplayChoices(index)
                        editBookmarkDialog.pcIndex = index
                        editBookmarkDialog.originalAddress = model.address
                        editBookmarkDialog.originalNickname = model.name
                        editBookmarkDialog.choices = choices
                        editBookmarkDialog.choiceIndex = computerModel.stationConnectDisplayChoice(index)
                        editBookmarkDialog.hostLayoutIndex =
                                computerModel.stationConnectHostLayoutChoice(index)
                        editBookmarkDialog.virtualModeIndex =
                                computerModel.stationConnectVirtualModeChoice(index)
                        editBookmarkDialog.originalProfile = computerModel.stationConnectVideoProfile(index)
                        editBookmarkDialog.open()
                    }
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
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
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.authorized) {
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

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
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
            TextField {
                id: usernameField
                placeholderText: qsTr("Username")
                Layout.fillWidth: true
                focus: true
            }
            TextField {
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
        property var choices: []
        property int choiceIndex: 0
        property int hostLayoutIndex: 0
        property int virtualModeIndex: 0
        property int originalProfile: StreamingPreferences.SCVP_H264_10BIT_444
        title: qsTr("Edit workstation bookmark")
        width: Math.min(640, parent.width - 40)
        dim: false
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            editAddressText.text = originalAddress
            editNicknameText.text = originalNickname
            editDisplayChoice.model = choices
            editDisplayChoice.currentIndex = choiceIndex
            editHostLayout.currentIndex = hostLayoutIndex
            editVirtualMode.currentIndex = virtualModeIndex
            for (var i = 0; i < editEncodingProfileModel.count; i++) {
                if (editEncodingProfileModel.get(i).val === originalProfile) {
                    editEncodingProfile.currentIndex = i
                    break
                }
            }
            editAddressText.forceActiveFocus()
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return editAddressText.text.trim() !== "" &&
                       editNicknameText.text.trim() !== "" &&
                       editDisplayChoice.currentIndex >= 0
            })
        }
        onClosed: {
            editAddressText.clear()
            editNicknameText.clear()
        }
        onAccepted: {
            if (!computerModel.editComputerBookmark(pcIndex,
                                                    editAddressText.text.trim(),
                                                    editNicknameText.text.trim(),
                                                    editDisplayChoice.currentIndex,
                                                    editHostLayout.currentIndex,
                                                    editVirtualMode.currentIndex,
                                                    editEncodingProfileModel.get(
                                                        editEncodingProfile.currentIndex).val)) {
                errorDialog.text = qsTr("Unable to update the workstation bookmark. Check the address and ensure another bookmark is not already using it.")
                errorDialog.helpText = ""
                errorDialog.open()
            }
        }

        ColumnLayout {
            width: parent.width

            Label {
                text: qsTr("Address or hostname")
                font.bold: true
            }
            TextField {
                id: editAddressText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Nickname")
                font.bold: true
            }
            TextField {
                id: editNicknameText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Encoding profile")
                font.bold: true
            }
            ComboBox {
                id: editEncodingProfile
                Layout.fillWidth: true
                textRole: "text"
                model: ListModel {
                    id: editEncodingProfileModel
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
                }
            }

            Label {
                text: qsTr("Host display layout")
                font.bold: true
            }
            ComboBox {
                id: editHostLayout
                Layout.fillWidth: true
                model: [
                    qsTr("Use the host's configured layout"),
                    qsTr("Physical displays"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                text: qsTr("Virtual display resolution")
                font.bold: true
                opacity: editHostLayout.currentIndex >= 2 ? 1.0 : 0.5
            }
            ComboBox {
                id: editVirtualMode
                Layout.fillWidth: true
                enabled: editHostLayout.currentIndex >= 2
                model: [qsTr("1920×1080 per display"), qsTr("3840×2160 per display")]
            }

            Label {
                text: qsTr("Client presentation")
                font.bold: true
            }
            ComboBox {
                id: editDisplayChoice
                Layout.fillWidth: true
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Named host monitors become available after StationConnect has retrieved its layout. A requested host layout must already be active.")
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

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
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

            TextField {
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

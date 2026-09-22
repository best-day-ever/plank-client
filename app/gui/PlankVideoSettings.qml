import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.3
import StreamingPreferences 1.0

// Capture source, encoding profile and startup encoder target(s), shared by
// the Add and Edit bookmark dialogs, the remote workstation settings and the
// remote access defaults, so every place offers exactly the same choices.
// Startup targets are kept per encoding profile; with showRoutes there is one
// list for the office network and one for the internet (remote access).
ColumnLayout {
    id: root

    PlankTheme {
        id: theme
    }

    // Probing (bookmarks): the capture choices follow the host's platform.
    property string hostAddress: ""
    property bool probingEnabled: false
    // Known platform without probing (1 = Linux, 2 = Mac, 0 = unknown).
    property int hostPlatform: 0
    // Profile picked when NvFBC capture is (re)selected.
    property int nvfbcDefaultProfile: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
    property bool showRoutes: false
    // Optional function(captureSource, videoProfile) returning why the host
    // cannot use a choice ("" when it can).
    property var profileProblem: null
    // Bumped by load() and refreshProblems(): profileProblem may read state
    // QML cannot track (e.g. capabilities cached in QSettings), so every
    // load re-asks it instead of keeping an answer from an earlier opening.
    property int problemRevision: 0

    property var officeBitratesKbps: []
    property var internetBitratesKbps: []
    readonly property int captureSource: captureBox.captureSource
    property int videoProfile: nvfbcDefaultProfile
    readonly property string problemText: problemFor(captureSource, videoProfile)

    // Any user change (not load()).
    signal edited()
    // The user picked another encoding profile or capture source.
    signal profileActivated()
    signal captureSourceEdited()

    property bool loading: false
    // Set once load() ran, so the initial default below never replaces a
    // loaded choice (a parent may load before this component completes).
    property bool loaded: false

    spacing: 6

    function problemFor(capture, profile) {
        // Read so bindings re-evaluate when the revision changes.
        var revision = problemRevision
        return profileProblem ? profileProblem(capture, profile) : ""
    }

    function refreshProblems() {
        problemRevision = problemRevision + 1
    }

    function copyList(values) {
        var result = []
        if (values) {
            for (var i = 0; i < values.length; ++i) result.push(values[i])
        }
        return result
    }

    function modelFor(capture) {
        return capture === StreamingPreferences.PLANK_CAPTURE_SCREENCAPTUREKIT ? appleProfiles :
               capture === StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT ? nvfbcProfiles : nativeProfiles
    }

    function bitrateFor(values, profile) {
        var saved = values ? values[profile] : undefined
        return saved === undefined ? StreamingPreferences.plankDefaultBitrateKbps(profile) : saved
    }

    function applyBitrates() {
        officeSlider.value = bitrateFor(officeBitratesKbps, videoProfile)
        internetSlider.value = bitrateFor(internetBitratesKbps, videoProfile)
    }

    // Selects profile in the current capture source's list (its first entry
    // when the list does not have it).
    function selectProfile(profile) {
        var profiles = modelFor(captureBox.captureSource)
        if (profileBox.model !== profiles) profileBox.model = profiles
        var index = 0
        for (var i = 0; i < profiles.count; ++i) {
            if (profiles.get(i).val === profile) index = i
        }
        profileBox.currentIndex = index
        videoProfile = profiles.get(index).val
        applyBitrates()
    }

    function load(capture, profile, officeBitrates, internetBitrates) {
        loading = true
        officeBitratesKbps = copyList(officeBitrates)
        internetBitratesKbps = copyList(internetBitrates)
        captureBox.selectCaptureSource(capture)
        selectProfile(profile)
        loaded = true
        loading = false
        refreshProblems()
    }

    function remember(values, value) {
        var result = copyList(values)
        while (result.length < videoProfile) {
            result.push(StreamingPreferences.plankDefaultBitrateKbps(result.length))
        }
        result[videoProfile] = Math.round(value)
        return result
    }

    Component.onCompleted: {
        if (!loaded) selectProfile(nvfbcDefaultProfile)
    }

    Label {
        text: qsTr("Capture source")
        font.bold: true
    }
    PlankCaptureSourceBox {
        id: captureBox
        objectName: "captureSource"
        Layout.fillWidth: true
        hostAddress: root.hostAddress
        probingEnabled: root.probingEnabled
        fixedPlatform: root.hostPlatform
        onCaptureSourceChanged: {
            if (root.loading) return
            root.selectProfile(captureSource === StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT ?
                                   root.nvfbcDefaultProfile : -1)
            root.captureSourceEdited()
            root.edited()
        }
    }

    Label {
        text: qsTr("Encoding profile")
        font.bold: true
    }
    PlankComboBox {
        id: profileBox
        objectName: "encodingProfile"
        Layout.fillWidth: true
        textRole: "text"
        delegate: ItemDelegate {
            width: profileBox.width
            text: model.text + (root.problemFor(captureBox.captureSource, model.val) !== "" ?
                                    qsTr(" (not offered by this workstation)") : "")
            highlighted: profileBox.highlightedIndex === index
        }
        onActivated: {
            root.videoProfile = model.get(currentIndex).val
            root.applyBitrates()
            root.profileActivated()
            root.edited()
        }
    }
    Label {
        Layout.fillWidth: true
        visible: root.problemText !== ""
        text: root.problemText
        color: theme.warning
        wrapMode: Text.Wrap
    }

    ListModel {
        id: nvfbcProfiles
        ListElement {
            text: qsTr("H.264 8-bit 4:2:2")
            val: StreamingPreferences.PLANK_PROFILE_H264_8BIT_422
        }
        ListElement {
            text: qsTr("H.264 8-bit 4:4:4 (identity GBR)")
            val: StreamingPreferences.PLANK_PROFILE_H264_8BIT_444
        }
        ListElement {
            text: qsTr("H.264 10-bit 4:2:2")
            val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_422
        }
        ListElement {
            text: qsTr("H.264 10-bit 4:4:4 (identity GBR)")
            val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
        }
        ListElement {
            text: qsTr("H.264 8-bit 4:4:4 (identity GBR) — NVENC")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_H264_8BIT_444
        }
        ListElement {
            text: qsTr("H.265 8-bit 4:4:4 (identity GBR) — NVENC")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_8BIT_444
        }
        ListElement {
            text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
        }
        ListElement {
            text: qsTr("H.264 8-bit 4:2:0 — NVENC (low bandwidth)")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_H264_8BIT_420
        }
        ListElement {
            text: qsTr("H.265 10-bit 4:2:0 — NVENC (low bandwidth)")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_420
        }
    }

    ListModel {
        id: appleProfiles
        ListElement {
            text: qsTr("HEVC 10-bit 4:2:0 — Apple VideoToolbox (Preview)")
            val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_420
        }
        ListElement {
            text: qsTr("HEVC 10-bit 4:4:4 — Apple VideoToolbox (Preview)")
            val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_444
        }
    }

    ListModel {
        id: nativeProfiles
        ListElement {
            text: qsTr("H.264 10-bit 4:4:4 (identity GBR) — x264")
            val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
        }
        ListElement {
            text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
            val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
        }
    }

    Label {
        text: (root.showRoutes ? qsTr("Startup encoder target on the office network: %1 Mbps") :
                                 qsTr("Startup encoder target: %1 Mbps")).arg((officeSlider.value / 1000.0).toFixed(1))
        font.bold: true
    }
    Slider {
        id: officeSlider
        Layout.fillWidth: true
        from: StreamingPreferences.plankBitrateMinimumKbps()
        to: StreamingPreferences.plankBitrateMaximumKbps()
        stepSize: StreamingPreferences.plankBitrateStepKbps()
        snapMode: Slider.SnapAlways
        onMoved: {
            root.officeBitratesKbps = root.remember(root.officeBitratesKbps, value)
            root.edited()
        }
    }

    Label {
        visible: root.showRoutes
        text: qsTr("Startup encoder target over the internet: %1 Mbps").arg((internetSlider.value / 1000.0).toFixed(1))
        font.bold: true
    }
    Slider {
        id: internetSlider
        visible: root.showRoutes
        Layout.fillWidth: true
        from: StreamingPreferences.plankBitrateMinimumKbps()
        to: StreamingPreferences.plankBitrateMaximumKbps()
        stepSize: StreamingPreferences.plankBitrateStepKbps()
        snapMode: Slider.SnapAlways
        onMoved: {
            root.internetBitratesKbps = root.remember(root.internetBitratesKbps, value)
            root.edited()
        }
    }

    Label {
        Layout.fillWidth: true
        text: root.showRoutes ?
                  qsTr("Saved independently for each encoding profile. The remote access server decides the route: the office network connects to the workstation directly, the internet goes through its relay. Toolbar adjustments apply only to the active session.") :
                  qsTr("Saved independently for each encoding profile. Toolbar adjustments apply only to the active session.")
        wrapMode: Text.Wrap
        opacity: 0.72
    }
}

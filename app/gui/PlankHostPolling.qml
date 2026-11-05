import QtQuick 2.9

// Visibility starts discovery even when the OS has not activated the window.
// A background launcher gets one bounded discovery period; hiding for a stream
// stops it immediately, while returning focus renews normal polling.
QtObject {
    id: lifecycle
    property var manager
    property bool windowVisible: false
    property bool windowActive: false
    property bool pollingActive: false
    property int inactivityInterval: 5 * 60000
    property bool initialized: false

    function startPolling() {
        if (!pollingActive && manager) {
            manager.startPolling()
            pollingActive = true
        }
    }
    function stopPolling() {
        if (pollingActive && manager) {
            manager.stopPollingAsync()
            pollingActive = false
        }
    }
    function updateVisibility() {
        if (!initialized) return
        if (!windowVisible) {
            inactivityTimer.stop()
            stopPolling()
        } else {
            startPolling()
            if (windowActive) inactivityTimer.stop()
            else inactivityTimer.restart()
        }
    }
    property Timer inactivityTimer: Timer {
        interval: lifecycle.inactivityInterval
        onTriggered: {
            if (!lifecycle.windowActive) lifecycle.stopPolling()
        }
    }
    onWindowVisibleChanged: updateVisibility()
    onWindowActiveChanged: {
        if (!initialized || !windowVisible) return
        if (windowActive) {
            inactivityTimer.stop()
            startPolling()
        } else inactivityTimer.restart()
    }
    Component.onCompleted: {
        initialized = true
        updateVisibility()
    }
    Component.onDestruction: stopPolling()
}

import QtQuick 2.15

// Draws a QR code from Onboarding.qrSize / qrModules ('1' = dark), black on
// white with the standard four-module quiet zone and whole-pixel modules so
// phone cameras read it reliably.
Canvas {
    id: qrCanvas

    property int modules: 0
    property string pattern: ""

    implicitWidth: 250
    implicitHeight: 250

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("QR code for your authenticator app")

    onModulesChanged: requestPaint()
    onPatternChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        ctx.fillStyle = "#ffffff"
        ctx.fillRect(0, 0, width, height)
        if (modules <= 0 || pattern.length !== modules * modules) {
            return
        }
        var quiet = 4
        var total = modules + 2 * quiet
        var cell = Math.max(1, Math.floor(Math.min(width, height) / total))
        var originX = Math.floor((width - cell * total) / 2) + quiet * cell
        var originY = Math.floor((height - cell * total) / 2) + quiet * cell
        ctx.fillStyle = "#000000"
        for (var y = 0; y < modules; ++y) {
            for (var x = 0; x < modules; ++x) {
                if (pattern.charAt(y * modules + x) === "1") {
                    ctx.fillRect(originX + x * cell, originY + y * cell, cell, cell)
                }
            }
        }
    }
}

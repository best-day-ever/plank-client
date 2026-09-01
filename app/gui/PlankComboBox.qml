import QtQuick 2.9
import QtQuick.Controls 2.2

ComboBox {
    id: control

    PlankTheme {
        id: theme
    }

    leftPadding: 12
    rightPadding: 34
    implicitHeight: 38

    contentItem: Text {
        text: control.displayText
        color: control.enabled ? theme.textPrimary : theme.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Canvas {
        width: 10
        height: 6
        x: control.width - control.rightPadding + 6
        anchors.verticalCenter: parent.verticalCenter
        contextType: "2d"

        onPaint: {
            context.reset()
            context.moveTo(1, 1)
            context.lineTo(width / 2, height - 1)
            context.lineTo(width - 1, 1)
            context.lineWidth = 1.5
            context.strokeStyle = control.enabled ? theme.textSecondary : theme.textDisabled
            context.stroke()
        }
    }

    background: Rectangle {
        color: control.down ? theme.surfacePressed : theme.surfaceRaised
        radius: theme.radiusSmall
        border.width: 1
        border.color: control.visualFocus ? theme.accent : theme.border
    }
}

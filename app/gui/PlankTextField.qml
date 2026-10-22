import QtQuick 2.9
import QtQuick.Controls 2.2

TextField {
    id: control

    PlankTheme {
        id: theme
    }

    leftPadding: 12
    rightPadding: 12
    implicitHeight: 38
    color: theme.textPrimary
    // Material floats the placeholder onto the top border once the field has
    // focus or text, which collides with this flat outline. Draw a plain hint
    // inside the field instead, shown only while the field is empty.
    placeholderTextColor: "transparent"
    selectionColor: theme.accent
    selectedTextColor: "white"

    Text {
        x: control.leftPadding
        y: control.topPadding
        width: control.width - control.leftPadding - control.rightPadding
        height: control.height - control.topPadding - control.bottomPadding
        visible: control.length === 0 && control.preeditText === ""
        text: control.placeholderText
        font: control.font
        color: theme.textDisabled
        verticalAlignment: control.verticalAlignment
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        color: theme.surfaceRaised
        radius: theme.radiusSmall
        border.width: 1
        border.color: control.activeFocus ? theme.accent : theme.border
    }
}

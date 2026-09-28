import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Controls.Material 2.15

Dialog {
    id: control

    PlankTheme {
        id: theme
    }

    // We should use Overlay.overlay here but that's not available in Qt 5.9 :(
    parent: ApplicationWindow.contentItem

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    padding: 20

    background: Rectangle {
        color: theme.surfaceRaised
        radius: theme.radiusLarge
        border.width: 1
        border.color: theme.border
    }

    // The Material header and footer draw their own, separately rounded
    // backgrounds in a different shade: a visible band across the title and
    // corners that look like resize handles. Keep both flush with the dialog.
    header: Label {
        visible: control.title !== ""
        text: control.title
        textFormat: Text.PlainText
        color: theme.textPrimary
        font.pointSize: 15
        font.weight: Font.DemiBold
        elide: Label.ElideRight
        topPadding: control.padding
        leftPadding: control.padding
        rightPadding: control.padding
        bottomPadding: 0
    }

    footer: DialogButtonBox {
        visible: count > 0
        leftPadding: control.padding - 6
        rightPadding: control.padding - 6
        topPadding: 4
        bottomPadding: 10

        // Material paints every footer button as accent-coloured text, so
        // Cancel and OK look alike. As on the pages, the action that accepts
        // is the one filled button and the others are plain text.
        Material.foreground: theme.textPrimary
        delegate: Button {
            highlighted: DialogButtonBox.buttonRole === DialogButtonBox.AcceptRole ||
                         DialogButtonBox.buttonRole === DialogButtonBox.YesRole
            flat: !highlighted
        }

        background: Item {}
    }

    // The Material dim is a light wash that bleaches the dark launcher; a
    // dark scrim (black 50%, the BDE styleguide's) lets it step back instead.
    Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.5)
    }
    Overlay.modeless: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.3)
    }

    onAboutToHide: {
        // We must force focus back to the last item for platforms without
        // support for more than one active window like Steam Link. If
        // we don't, keyboard navigation will break after a
        // dialog appears.
        stackView.forceActiveFocus()
    }
}

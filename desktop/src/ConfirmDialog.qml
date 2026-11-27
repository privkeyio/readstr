import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks before something goes: `title`, then `message`, with Cancel and
// `confirmText`, which Return or Enter presses too, and which emits
// `confirmed` before the dialog closes.
Dialog {
    id: dialog

    property string message: ""
    property string confirmText: ""
    property string confirmObjectName: ""

    signal confirmed()

    function confirm() {
        confirmed()
        close()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 460)
    modal: true
    Material.roundedScale: theme.radius

    // Drawn plainly rather than through Material's elevation layer, which
    // the software scene graph leaves out.
    background: Rectangle {
        radius: theme.radius
        color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.06))
        border.color: Qt.alpha(theme.foreground, 0.15)
        border.width: 1
    }

    // The title sits in an Item whose implicitHeight reads the label's at
    // once. A bare Label works out its implicit size only when first resized,
    // and when that happens while the dialog is setting its implicitHeight
    // (a window resize rewrapping the content) the binding loops.
    header: Item {
        implicitWidth: heading.implicitWidth
        implicitHeight: heading.implicitHeight

        Label {
            id: heading
            width: parent.width
            text: dialog.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            padding: 24
            bottomPadding: 0
            font.pixelSize: 20
        }
    }

    contentItem: Label {
        text: dialog.message
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        maximumLineCount: 6
        elide: Text.ElideRight
    }

    Shortcut { sequences: ["Return", "Enter"]; enabled: dialog.visible; context: Qt.ApplicationShortcut; onActivated: dialog.confirm() }

    footer: DialogButtonBox {
        background: null
        Button {
            text: qsTr("Cancel")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
        Button {
            objectName: dialog.confirmObjectName
            text: dialog.confirmText
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.confirm()
        }
    }
}

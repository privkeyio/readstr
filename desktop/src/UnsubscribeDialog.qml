import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks before a feed and its articles leave the library.
Dialog {
    id: dialog

    property var feedId: 0
    property string feedTitle: ""

    function ask(id, name) {
        feedId = id
        feedTitle = name
        open()
    }

    function confirm() {
        backend.unsubscribe(feedId)
        close()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 460)
    modal: true
    title: qsTr("Unsubscribe?")
    Material.roundedScale: theme.radius

    // Drawn plainly rather than through Material's elevation layer, which
    // the software scene graph leaves out.
    background: Rectangle {
        radius: theme.radius
        color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.06))
        border.color: Qt.alpha(theme.foreground, 0.15)
        border.width: 1
    }

    header: Label {
        text: dialog.title
        textFormat: Text.PlainText
        elide: Text.ElideRight
        padding: 24
        bottomPadding: 0
        font.pixelSize: 20
    }

    contentItem: Label {
        text: qsTr("%1 and its articles will be removed from the library.").arg(dialog.feedTitle)
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
            text: qsTr("Unsubscribe")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.confirm()
        }
    }
}

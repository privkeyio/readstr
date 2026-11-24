import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The settings kept in settings.json. Each change is saved at once.
Dialog {
    id: dialog
    objectName: "settingsDialog"

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 520)
    modal: true
    title: qsTr("Settings")
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

    contentItem: ColumnLayout {
        spacing: 8

        Switch {
            objectName: "loadRemoteImages"
            Layout.fillWidth: true
            text: qsTr("Load remote images")
            checked: backend.settings.loadRemoteImages
            focusPolicy: Qt.NoFocus
            onToggled: backend.settings.loadRemoteImages = checked
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Shows the images of the article you open. Fetching them tells their sites that you read it. When this is off, images are links, and an article's images can be loaded for it alone.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            opacity: 0.7
        }

        Label {
            Layout.fillWidth: true
            visible: backend.settings.error !== ""
            text: backend.settings.error
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
            color: dialog.errorColor
        }
    }

    footer: DialogButtonBox {
        background: null
        Button {
            text: qsTr("Close")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
    }
}

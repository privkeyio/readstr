import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Subscribes to a feed address, or finds the feed behind a site's address.
// It stays open while the feed is looked for and closes once subscribed.
Dialog {
    id: dialog

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)

    function submit() {
        if (field.text.trim() !== "" && !backend.adding)
            backend.addFeed(field.text)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 520)
    modal: true
    title: qsTr("Add a feed")
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

    onOpened: {
        if (!backend.adding)
            field.text = ""
        backend.clearAddError()
        field.forceActiveFocus()
    }

    Connections {
        target: backend
        function onSubscribed() { dialog.close() }
    }

    contentItem: ColumnLayout {
        spacing: 12

        TextField {
            id: field
            Layout.fillWidth: true
            placeholderText: qsTr("Feed, site, or npub")
            enabled: !backend.adding
            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
            Material.roundedScale: theme.radius
            onAccepted: dialog.submit()
            onTextEdited: backend.clearAddError()
        }

        RowLayout {
            Layout.fillWidth: true
            visible: backend.adding
            spacing: 10

            BusyIndicator {
                running: backend.adding
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Looking for the feed…")
                textFormat: Text.PlainText
                elide: Text.ElideRight
                opacity: 0.7
            }
        }

        Label {
            Layout.fillWidth: true
            visible: backend.addError !== ""
            text: backend.addError
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
            text: qsTr("Cancel")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
        Button {
            text: qsTr("Add")
            flat: true
            focusPolicy: Qt.NoFocus
            enabled: field.text.trim() !== "" && !backend.adding
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

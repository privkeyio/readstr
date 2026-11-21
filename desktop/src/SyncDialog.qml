import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sets or clears the npub whose subscriptions, read articles, and saved
// views come from the Readstr web app. Only the public key is kept: the
// events are public, so reading them needs no signer.
Dialog {
    id: dialog

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    property bool invalid: false

    function submit() {
        if (field.text.trim() === "")
            return
        if (backend.setSyncNpub(field.text))
            close()
        else
            invalid = true
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 520)
    modal: true
    title: qsTr("Sync with Nostr")
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

    onOpened: {
        field.text = backend.syncNpub
        invalid = false
        field.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("After every refresh, Readstr reads the subscriptions and saved views the Readstr web app publishes for this npub, and its read articles once it publishes them. It never signs or publishes anything.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            opacity: 0.7
        }

        TextField {
            id: field
            Layout.fillWidth: true
            placeholderText: qsTr("npub or nprofile")
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            Material.roundedScale: theme.radius
            onAccepted: dialog.submit()
            onTextEdited: dialog.invalid = false
        }

        Label {
            Layout.fillWidth: true
            visible: backend.syncNpub !== "" && !!backend.syncedAt
            text: {
                translator.language
                const date = backend.syncedAt
                return date ? qsTr("Last synced %1").arg(date.toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
            }
            textFormat: Text.PlainText
            elide: Text.ElideRight
            opacity: 0.7
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.invalid
            text: qsTr("That is not an npub or nprofile")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: dialog.errorColor
        }
    }

    footer: DialogButtonBox {
        background: null
        Button {
            text: qsTr("Stop syncing")
            flat: true
            focusPolicy: Qt.NoFocus
            visible: backend.syncNpub !== ""
            Material.roundedScale: theme.radius
            onClicked: {
                backend.stopSync()
                dialog.close()
            }
        }
        Button {
            text: qsTr("Cancel")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
        Button {
            text: qsTr("Sync")
            flat: true
            focusPolicy: Qt.NoFocus
            enabled: field.text.trim() !== "" && !backend.syncing
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

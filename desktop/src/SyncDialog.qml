import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sets or clears the npub whose subscriptions, read articles, and saved
// views come from the Readstr web app. The events are public, so reading
// them needs only the public key. The account's secret key can be added for
// signing later; it goes to the keyring alone, and the field lets go of it as
// soon as it is submitted or the dialog closes. That is as far as QML goes:
// the JavaScript strings it passed through are freed by the garbage
// collector, not wiped.
Dialog {
    id: dialog

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    property bool invalid: false
    // Asking whether "Stop syncing" also removes the stored signing key.
    property bool confirmingStop: false

    function submit() {
        if (field.text.trim() === "")
            return
        if (backend.setSyncNpub(field.text))
            close()
        else
            invalid = true
    }

    function stopSyncing(removeKey) {
        backend.stopSync(removeKey)
        close()
    }

    function askToStop() {
        if (!backend.signingKeyStored)
            return stopSyncing(false)
        confirmingStop = true
        confirmation.forceActiveFocus()
    }

    function addKey() {
        const key = keyField.text
        clearKey()
        if (key.trim() !== "")
            backend.addSigningKey(key)
    }

    // Set rather than clear()ed: clear() is an edit, which the field's undo
    // history keeps, so Ctrl+Z would bring the key back even in a password
    // field, while setting the text starts that history afresh.
    function clearKey() {
        keyField.text = ""
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
        confirmingStop = false
        clearKey()
        backend.checkSigningKey()
        field.forceActiveFocus()
    }
    onClosed: {
        clearKey()
        confirmingStop = false
    }

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            visible: !dialog.confirmingStop
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("After every refresh, Readstr reads the subscriptions and saved views the Readstr web app publishes for this npub, and its read articles once it publishes them. Readstr itself publishes nothing yet.")
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

            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: backend.syncNpub !== ""
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Signing key")
                    textFormat: Text.PlainText
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: backend.signingKeyStored
                          ? qsTr("Readstr can sign as this account: its key is in your keyring.")
                          : qsTr("Add this account's nsec so Readstr can sign as it. The key is kept only in your keyring.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    opacity: 0.7
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: !backend.signingKeyStored
                    spacing: 8

                    TextField {
                        id: keyField
                        objectName: "signingKeyField"
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: qsTr("nsec or hex secret key")
                        inputMethodHints: Qt.ImhSensitiveData | Qt.ImhHiddenText | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        Material.roundedScale: theme.radius
                        onAccepted: dialog.addKey()
                    }
                    Button {
                        objectName: "addSigningKeyButton"
                        text: qsTr("Add signing key")
                        flat: true
                        focusPolicy: Qt.NoFocus
                        enabled: keyField.text !== ""
                        Material.roundedScale: theme.radius
                        onClicked: dialog.addKey()
                    }
                }

                Button {
                    objectName: "forgetSigningKeyButton"
                    text: qsTr("Forget key")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    visible: backend.signingKeyStored
                    Material.roundedScale: theme.radius
                    onClicked: backend.forgetSigningKey()
                }

                Label {
                    Layout.fillWidth: true
                    visible: backend.signingBusy
                    text: qsTr("Waiting for the keyring…")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    opacity: 0.7
                }

                Label {
                    objectName: "signingErrorLabel"
                    Layout.fillWidth: true
                    visible: backend.signingError !== ""
                    text: backend.signingError
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: dialog.errorColor
                }
            }
        }

        // Asked in place of the form, as there is no confirm dialog to
        // borrow; Enter takes the default, removing the key.
        ColumnLayout {
            id: confirmation
            objectName: "stopSyncConfirmation"
            Layout.fillWidth: true
            visible: dialog.confirmingStop
            spacing: 12
            Keys.onReturnPressed: dialog.stopSyncing(true)
            Keys.onEnterPressed: dialog.stopSyncing(true)

            Label {
                Layout.fillWidth: true
                text: qsTr("Also remove this account's signing key from your keyring? Syncing stops either way.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
            }

            // Here rather than in the footer, whose DialogButtonBox resets
            // the highlight that marks the default.
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8

                Button {
                    objectName: "keepKeyButton"
                    text: qsTr("Keep key")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    onClicked: dialog.stopSyncing(false)
                }
                Button {
                    objectName: "removeKeyButton"
                    text: qsTr("Remove key")
                    // Flat, as Material's raised fill is drawn through the
                    // elevation layer the software scene graph leaves out;
                    // flat and highlighted, it takes the accent color.
                    flat: true
                    highlighted: true
                    focusPolicy: Qt.NoFocus
                    Material.roundedScale: theme.radius
                    onClicked: dialog.stopSyncing(true)
                }
            }
        }
    }

    footer: DialogButtonBox {
        background: null
        Button {
            objectName: "stopSyncButton"
            text: qsTr("Stop syncing")
            flat: true
            focusPolicy: Qt.NoFocus
            visible: backend.syncNpub !== "" && !dialog.confirmingStop
            Material.roundedScale: theme.radius
            onClicked: dialog.askToStop()
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
            visible: !dialog.confirmingStop
            enabled: field.text.trim() !== "" && !backend.syncing
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

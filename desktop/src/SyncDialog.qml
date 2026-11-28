import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sets or clears the npub whose subscriptions, read articles, and saved
// views are synced with the Readstr web app. The events are public, so
// reading them needs only the public key; publishing this machine's own
// needs the account's secret key, which goes to the keyring alone, and the
// field lets go of it as soon as it is submitted or the dialog closes. That
// is as far as QML goes: the JavaScript strings it passed through are freed
// by the garbage collector, not wiped. A remote signer app (NIP-46) can sign
// instead, paired through a bunker:// link pasted here or a nostrconnect://
// link the app scans; only the pairing goes to the keyring.
Dialog {
    id: dialog

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    property bool invalid: false
    // Asking whether "Stop syncing" also removes the stored signing key.
    property bool confirmingStop: false
    // Asking whether another npub may delete the saved views of the last
    // one, or its category and tag changes not published yet, which nothing
    // brings back.
    property bool confirmingDrop: false
    readonly property bool asking: confirmingStop || confirmingDrop

    function submit() {
        if (field.text.trim() === "")
            return
        if (backend.syncNpubDropsViews(field.text)) {
            confirmingDrop = true
            scroller.ScrollBar.vertical.position = 0
            keepViewsButton.forceActiveFocus()
            return
        }
        syncWith(false)
    }

    function syncWith(dropViews) {
        confirmingDrop = false
        if (backend.setSyncNpub(field.text, dropViews))
            close()
        else
            invalid = true
    }

    function stopSyncing(removeKey) {
        backend.stopSync(removeKey)
        close()
    }

    // Asked unless the keyring has answered that it holds neither a key nor
    // a pairing: while a check is out, or after one failed, one may well be
    // there.
    function askToStop() {
        if (backend.signerMissing && !backend.signingBusy)
            return stopSyncing(false)
        confirmingStop = true
        scroller.ScrollBar.vertical.position = 0
        removeKeyButton.forceActiveFocus()
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
        bunkerField.text = ""
    }

    // The link's secret lets whoever holds it pair with the signer, so it
    // leaves the field the way a key does.
    function connectBunker() {
        const uri = bunkerField.text
        bunkerField.text = ""
        if (uri.trim() !== "")
            backend.connectBunker(uri)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 520)
    height: Math.min(implicitHeight, parent.height - 32)
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
        field.text = backend.syncNpub
        invalid = false
        confirmingStop = false
        confirmingDrop = false
        clearKey()
        backend.checkSigningKey()
        field.forceActiveFocus()
    }
    onClosed: {
        clearKey()
        confirmingStop = false
        confirmingDrop = false
    }

    // The form scrolls within a dialog no taller than the window, as the
    // pairing panel's code and link can take it past the window's bottom;
    // the footer's buttons stay in view below it.
    contentItem: ScrollView {
        id: scroller
        objectName: "syncDialogScroll"
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroller.availableWidth
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                visible: !dialog.asking
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("After every refresh, Readstr reads the subscriptions, read articles, and saved views the Readstr web app publishes for this npub. With the account's signing key, Readstr publishes its own subscriptions in return, and its read articles if you allow it, so a change here shows in the web app. What is published is public.")
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
                              ? qsTr("A signing key for this account is stored in your keyring.")
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

                    Label {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        text: qsTr("Remote signer")
                        textFormat: Text.PlainText
                        font.bold: true
                    }

                    Label {
                        Layout.fillWidth: true
                        text: {
                            if (!backend.remoteSignerPaired)
                                return qsTr("Or let a signer app such as Amber sign for this account: paste its bunker:// link, or show a link for the app to scan. Only the pairing is kept, in your keyring.")
                            if (backend.signingKeyStored)
                                return qsTr("A remote signer is paired too. The signing key in your keyring is used first.")
                            return qsTr("Readstr signs through your signer app. The pairing is kept in your keyring.")
                        }
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        opacity: 0.7
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: !backend.remoteSignerPaired && !backend.remoteConnecting
                        spacing: 8

                        TextField {
                            id: bunkerField
                            objectName: "bunkerField"
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: qsTr("bunker:// link")
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            Material.roundedScale: theme.radius
                            onAccepted: dialog.connectBunker()
                        }
                        Button {
                            objectName: "connectBunkerButton"
                            text: qsTr("Connect")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            enabled: bunkerField.text !== ""
                            Material.roundedScale: theme.radius
                            onClicked: dialog.connectBunker()
                        }
                    }

                    Button {
                        objectName: "nostrConnectButton"
                        text: qsTr("Connect with a signer app")
                        flat: true
                        focusPolicy: Qt.NoFocus
                        visible: !backend.remoteSignerPaired && !backend.remoteConnecting
                        Material.roundedScale: theme.radius
                        onClicked: backend.startNostrConnect()
                    }

                    ColumnLayout {
                        objectName: "nostrConnectPanel"
                        Layout.fillWidth: true
                        visible: backend.remoteConnecting
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            visible: backend.nostrConnectUri !== ""
                            text: qsTr("Scan this code with your signer app, or copy the link into it.")
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                        }

                        // Drawn in process from the link; white around the code
                        // keeps the quiet zone a scanner needs on a dark theme.
                        Image {
                            objectName: "nostrConnectQr"
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 200
                            Layout.preferredHeight: 200
                            visible: backend.nostrConnectQr !== ""
                            source: backend.nostrConnectQr
                            sourceSize: Qt.size(200, 200)
                            fillMode: Image.PreserveAspectFit
                            smooth: false
                            cache: false
                        }

                        TextEdit {
                            Layout.fillWidth: true
                            visible: backend.nostrConnectUri !== ""
                            text: backend.nostrConnectUri
                            textFormat: TextEdit.PlainText
                            wrapMode: TextEdit.WrapAnywhere
                            readOnly: true
                            selectByMouse: true
                            color: theme.foreground
                            opacity: 0.7
                            font.pixelSize: 11
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Waiting for your signer app…")
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            opacity: 0.7
                        }

                        RowLayout {
                            spacing: 8

                            Button {
                                objectName: "copyNostrConnectButton"
                                text: qsTr("Copy link")
                                flat: true
                                focusPolicy: Qt.NoFocus
                                visible: backend.nostrConnectUri !== ""
                                Material.roundedScale: theme.radius
                                onClicked: backend.copyNostrConnectUri()
                            }
                            Button {
                                objectName: "cancelRemoteConnectButton"
                                text: qsTr("Stop connecting")
                                flat: true
                                focusPolicy: Qt.NoFocus
                                Material.roundedScale: theme.radius
                                onClicked: backend.cancelRemoteConnect()
                            }
                        }
                    }

                    Button {
                        objectName: "disconnectSignerButton"
                        text: qsTr("Disconnect signer")
                        flat: true
                        focusPolicy: Qt.NoFocus
                        visible: backend.remoteSignerPaired
                        Material.roundedScale: theme.radius
                        onClicked: backend.disconnectSigner()
                    }

                    Label {
                        objectName: "remoteSignerWaitingLabel"
                        Layout.fillWidth: true
                        visible: backend.remoteSignerWaiting
                        text: qsTr("Approve the request in your signer app")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }

                    // Shown as text to open by hand, never opened from here.
                    Label {
                        Layout.fillWidth: true
                        visible: backend.remoteAuthUrl !== ""
                        text: qsTr("Your signer app asks you to approve the request at this address:")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                    }
                    TextEdit {
                        objectName: "remoteAuthUrl"
                        Layout.fillWidth: true
                        visible: backend.remoteAuthUrl !== ""
                        text: backend.remoteAuthUrl
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.WrapAnywhere
                        readOnly: true
                        selectByMouse: true
                        color: theme.foreground
                    }

                    Label {
                        objectName: "remoteSignerErrorLabel"
                        Layout.fillWidth: true
                        visible: backend.remoteSignerError !== ""
                        text: backend.remoteSignerError
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: dialog.errorColor
                    }

                    CheckBox {
                        objectName: "publishReadsBox"
                        Layout.fillWidth: true
                        text: qsTr("Also publish my read articles (public)")
                        checked: backend.publishReads
                        focusPolicy: Qt.NoFocus
                        onToggled: {
                            backend.setPublishReads(checked)
                            // Toggling broke the binding; the box shows what is stored.
                            checked = Qt.binding(() => backend.publishReads)
                        }
                    }

                    Label {
                        objectName: "confirmPublishLabel"
                        Layout.fillWidth: true
                        visible: !backend.signerMissing && !backend.publishConfirmed
                        text: qsTr("Changes are published after the first “Publish now” for this npub.")
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        opacity: 0.7
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: !backend.signerMissing
                        spacing: 8

                        Label {
                            objectName: "publishedLabel"
                            Layout.fillWidth: true
                            text: {
                                translator.language
                                if (backend.publishing)
                                    return qsTr("Publishing…")
                                const date = backend.publishedAt
                                return date ? qsTr("Last published %1").arg(date.toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
                            }
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            opacity: 0.7
                        }
                        Button {
                            objectName: "publishNowButton"
                            text: qsTr("Publish now")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            enabled: !backend.publishing
                            Material.roundedScale: theme.radius
                            onClicked: backend.publishNow()
                        }
                    }

                    Label {
                        objectName: "publishErrorLabel"
                        Layout.fillWidth: true
                        visible: backend.publishError !== ""
                        text: backend.publishError
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: dialog.errorColor
                    }
                }
            }

            // Asked in place of the form, as there is no confirm dialog to
            // borrow. Removing the key is the default, focused first; Tab moves
            // between the answers and Enter takes the focused one.
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
                    text: qsTr("Also remove this account's signing key and remote signer pairing from your keyring? Syncing stops either way.")
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
                        Material.roundedScale: theme.radius
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: dialog.stopSyncing(false)
                    }
                    Button {
                        id: removeKeyButton
                        objectName: "removeKeyButton"
                        text: qsTr("Remove key")
                        // Flat, as Material's raised fill is drawn through the
                        // elevation layer the software scene graph leaves out;
                        // flat and highlighted, it takes the accent color.
                        flat: true
                        highlighted: true
                        Material.roundedScale: theme.radius
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: dialog.stopSyncing(true)
                    }
                }
            }

            // Asked in place of the form, as for "Stop syncing". Keeping the
            // views is the default, focused first, as removing them cannot
            // be undone.
            ColumnLayout {
                objectName: "dropViewsConfirmation"
                Layout.fillWidth: true
                visible: dialog.confirmingDrop
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("The saved views here, or category and tag changes not published yet, came from another npub. Syncing with this one removes them from this computer, with the views' mute words and any edits not published yet, and that cannot be undone.")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 8

                    Button {
                        id: keepViewsButton
                        objectName: "keepViewsButton"
                        text: qsTr("Keep them")
                        flat: true
                        highlighted: true
                        Material.roundedScale: theme.radius
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: dialog.confirmingDrop = false
                    }
                    Button {
                        objectName: "dropViewsButton"
                        text: qsTr("Remove them")
                        flat: true
                        Material.roundedScale: theme.radius
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: dialog.syncWith(true)
                    }
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
            visible: backend.syncNpub !== "" && !dialog.asking
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
            visible: !dialog.asking
            enabled: field.text.trim() !== "" && !backend.syncing
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

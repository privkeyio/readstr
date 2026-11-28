import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Edits a feed's tags, a comma-separated list, or renames a tag on every
// feed that carries it. Either way the subscription list carries the
// change to the web app. The dialog closes once the change is saved, so a
// refused one keeps what was typed and says why.
Dialog {
    id: dialog
    objectName: "tagsDialog"

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    // "feed" for a feed's tags, "rename" for a tag.
    property string mode: "feed"
    property var feedId: 0
    property string feedTitle: ""
    // The tag renamed, as it shows, and how many feeds carry it.
    property string tag: ""
    property int feeds: 0
    // The text the form was filled in with, which saving as it is changes
    // nothing.
    property string initial: ""
    property bool attempted: false
    // As in ViewDialog: the form waits, locked, for its save to be written.
    property bool saving: false
    // The id of the request its save is waiting on (Backend::organizeSaved()),
    // so the answer to another request, such as a delete confirmed while
    // this one is out, neither closes it nor takes its place.
    property int request: 0
    property string saveError: ""

    readonly property string problem: mode === "rename" ? backend.tagProblem(field.text)
                                                        : backend.tagListProblem(field.text, feedId)

    function editFeed(id, title) {
        if (saving || !(id > 0))
            return
        mode = "feed"
        feedId = id
        feedTitle = title
        initial = backend.feedTags(id)
        fill()
    }

    function rename(name) {
        if (saving || name === "")
            return
        mode = "rename"
        tag = name
        feeds = backend.tagFeedCount(name)
        initial = name
        fill()
    }

    function fill() {
        field.text = initial
        attempted = false
        saveError = ""
        open()
    }

    function submit() {
        if (saving)
            return
        attempted = true
        if (problem !== "")
            return
        if (field.text.trim() === initial) {
            close()
            return
        }
        saving = true
        request = mode === "rename" ? backend.renameTag(tag, field.text) : backend.setFeedTags(feedId, field.text)
    }

    Connections {
        target: backend
        enabled: dialog.saving
        function onOrganizeSaved(request, ok, why) {
            if (request !== dialog.request)
                return
            dialog.request = 0
            dialog.saving = false
            if (ok) {
                dialog.close()
            } else {
                dialog.saveError = why
                field.forceActiveFocus()
            }
        }
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 460)
    modal: true
    closePolicy: saving ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: mode === "rename" ? qsTr("Rename the tag %1").arg(tag) : qsTr("Tags of %1").arg(feedTitle)
    Material.roundedScale: theme.radius

    // Drawn plainly rather than through Material's elevation layer, which
    // the software scene graph leaves out.
    background: Rectangle {
        radius: theme.radius
        color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.06))
        border.color: Qt.alpha(theme.foreground, 0.15)
        border.width: 1
    }

    // As in ViewDialog, so the title's height never loops with the dialog's.
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

    onOpened: field.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: 6
        enabled: !dialog.saving

        TextField {
            id: field
            objectName: "tagsField"
            Layout.fillWidth: true
            Layout.topMargin: 8
            placeholderText: dialog.mode === "rename" ? qsTr("Name") : qsTr("Tags, separated by commas")
            Accessible.name: placeholderText
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            Material.roundedScale: theme.radius
            onAccepted: dialog.submit()
            onTextChanged: dialog.saveError = ""
        }

        Label {
            Layout.fillWidth: true
            text: dialog.mode === "rename" ? qsTr("Renamed on the %n feeds that carry it.", "", dialog.feeds)
                                           : qsTr("Tags that look the same are kept once.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 11
            opacity: 0.7
        }

        Label {
            objectName: "tagsProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: {
                if (dialog.saveError !== "")
                    return dialog.saveError
                return dialog.attempted || dialog.mode === "feed" || field.text.trim() !== "" ? dialog.problem : ""
            }
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: dialog.errorColor
        }
    }

    footer: DialogButtonBox {
        background: null
        Button {
            objectName: "tagsCancel"
            text: qsTr("Cancel")
            enabled: !dialog.saving
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
        Button {
            objectName: "tagsSave"
            text: dialog.mode === "rename" ? qsTr("Rename") : qsTr("Save")
            enabled: !dialog.saving
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

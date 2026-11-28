import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Moves a feed to a category, an existing one, a new one typed in, or none;
// or renames a category, whose feeds join the category of the new name when
// there is one. Either way the subscription list carries the change to the
// web app. The dialog closes once the change is saved, so a refused one
// keeps what was typed and says why.
Dialog {
    id: dialog
    objectName: "categoryDialog"

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    // "move" for a feed, "rename" for a category.
    property string mode: "move"
    property var feedId: 0
    property string feedTitle: ""
    // The category renamed, or the one the feed is in, as the library keeps
    // it, and as it shows.
    property string category: ""
    property string categoryTitle: ""
    // The choices of a move: no category, each category, or a new one.
    property var choices: []
    // An empty name is only an error once the user tried to save.
    property bool attempted: false
    // As in ViewDialog: the form waits, locked, for its save to be written.
    property bool saving: false
    // The id of the request its save is waiting on (Backend::organizeSaved()),
    // so the answer to another request, such as a delete confirmed while
    // this one is out, neither closes it nor takes its place.
    property int request: 0
    property string saveError: ""

    readonly property bool typing: mode === "rename" || (choiceBox.currentIndex >= 0
                                                         && choiceBox.currentIndex < choices.length
                                                         && choices[choiceBox.currentIndex].kind === "new")
    // The category asked for, empty for none.
    readonly property string chosenName: {
        if (typing)
            return nameField.text.trim()
        const choice = choices[choiceBox.currentIndex]
        return choice && choice.kind === "name" ? choice.name : ""
    }
    readonly property string problem: typing ? backend.categoryProblem(nameField.text) : ""
    // A rename onto a category there already merges the two.
    readonly property bool merges: mode === "rename" && chosenName !== category
                                   && backend.categoryNames().indexOf(chosenName) >= 0
    onChosenNameChanged: saveError = ""

    function moveFeed(id, title) {
        if (saving || !(id > 0))
            return
        mode = "move"
        feedId = id
        feedTitle = title
        category = backend.feedCategory(id)
        const names = backend.categoryNames()
        const list = [{ kind: "none", name: "", text: qsTr("No category") }]
        let current = 0
        for (let i = 0; i < names.length; ++i) {
            if (names[i] === category)
                current = list.length
            list.push({ kind: "name", name: names[i], text: names[i] })
        }
        list.push({ kind: "new", name: "", text: qsTr("New category…") })
        choices = list
        choiceBox.currentIndex = current
        nameField.text = ""
        fill()
    }

    function rename(name, title) {
        if (saving || name === "")
            return
        mode = "rename"
        category = name
        categoryTitle = title
        choices = []
        nameField.text = name
        fill()
    }

    function fill() {
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
        // Asked for as it is: nothing to change.
        if (chosenName === category) {
            close()
            return
        }
        saving = true
        request = mode === "rename" ? backend.renameCategory(category, chosenName)
                                    : backend.setFeedCategory(feedId, chosenName)
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
                if (dialog.typing)
                    nameField.forceActiveFocus()
            }
        }
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 460)
    modal: true
    closePolicy: saving ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    title: mode === "rename" ? qsTr("Rename the category %1").arg(categoryTitle)
                             : qsTr("Move %1 to a category").arg(feedTitle)
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

    onOpened: {
        if (typing)
            nameField.forceActiveFocus()
        else
            choiceBox.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 6
        enabled: !dialog.saving

        // The names come from feeds and the web app, so they show as plain
        // text, which the default delegate would not keep them.
        ComboBox {
            id: choiceBox
            objectName: "categoryChoice"
            Layout.fillWidth: true
            Layout.topMargin: 8
            visible: dialog.mode === "move"
            model: dialog.choices
            textRole: "text"
            Accessible.name: qsTr("Category")
            Material.roundedScale: theme.radius
            onActivated: {
                if (dialog.typing)
                    nameField.forceActiveFocus()
            }
            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: ListView.view ? ListView.view.width : implicitWidth
                highlighted: choiceBox.highlightedIndex === index
                contentItem: Label {
                    text: modelData.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        TextField {
            id: nameField
            objectName: "categoryName"
            Layout.fillWidth: true
            Layout.topMargin: dialog.mode === "rename" ? 8 : 0
            visible: dialog.typing
            placeholderText: qsTr("Name")
            maximumLength: backend.maxCategoryNameLength
            Accessible.name: qsTr("Name")
            Material.roundedScale: theme.radius
            onAccepted: dialog.submit()
        }

        Label {
            objectName: "categoryMerges"
            Layout.fillWidth: true
            visible: dialog.merges
            text: qsTr("%1 is a category already: its feeds and these will be one category.").arg(dialog.chosenName)
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 11
            opacity: 0.7
        }

        Label {
            objectName: "categoryProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: {
                if (dialog.saveError !== "")
                    return dialog.saveError
                return dialog.attempted || nameField.text.trim() !== "" ? dialog.problem : ""
            }
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: dialog.errorColor
        }
    }

    // Return saves from the list too, which takes no Enter of its own.
    Shortcut { sequences: ["Return", "Enter"]; enabled: dialog.visible && !dialog.typing && !choiceBox.popup.visible; context: Qt.ApplicationShortcut; onActivated: dialog.submit() }

    footer: DialogButtonBox {
        background: null
        Button {
            objectName: "categoryCancel"
            text: qsTr("Cancel")
            enabled: !dialog.saving
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
        Button {
            objectName: "categorySave"
            text: dialog.mode === "rename" ? qsTr("Rename") : qsTr("Move")
            enabled: !dialog.saving
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

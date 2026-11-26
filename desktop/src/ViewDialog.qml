import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Creates a saved view, or edits one: its name and icon, what it lists (all
// articles, starred ones, or the feeds of tags), which of them and in which
// order, the keywords it looks for, and the mute words it hides, which stay
// on this computer. A view of one feed or one category keeps that source,
// which only the web app can set. A view saved is published as the web
// app's views are, without its mute words.
Dialog {
    id: dialog
    objectName: "viewDialog"

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    // The view edited, or "" while the dialog makes a new one.
    property string viewId: ""
    property bool sourceEditable: true
    // The source of a view whose source cannot be edited, as it is.
    property string fixedSource: ""
    // The tags offered, and those ticked.
    property var tags: []
    property var picked: []
    // An empty name is only an error once the user tried to save.
    property bool attempted: false

    readonly property var form: ({
        name: nameField.text,
        icon: iconField.text,
        source: sourceEditable ? sourceBox.currentValue : fixedSource,
        tags: picked,
        readState: readBox.currentValue,
        sort: sortBox.currentValue,
        include: includeField.text,
        exclude: excludeField.text
    })
    readonly property string problem: {
        // Read so the problem is worked out again when the views change, as
        // a pull meanwhile can fill the list or remove this view.
        backend.viewCount
        return backend.viewProblem(viewId, form)
    }

    readonly property var sources: [
        { value: "all", text: qsTr("All articles") },
        { value: "favorites", text: qsTr("Starred") },
        { value: "tags", text: qsTr("Tags") }
    ]
    readonly property var readStates: [
        { value: "all", text: qsTr("All") },
        { value: "unread", text: qsTr("Unread") },
        { value: "read", text: qsTr("Read") }
    ]
    readonly property var sorts: [
        { value: "newest", text: qsTr("Newest first") },
        { value: "oldest", text: qsTr("Oldest first") }
    ]

    // Opens on `form` (Backend::viewForm()) for the view `id`, or for a new
    // view when `id` is "".
    function load(id, values) {
        if (!values || values.name === undefined)
            return
        viewId = id
        attempted = false
        sourceEditable = values.sourceEditable
        fixedSource = values.sourceEditable ? "" : values.source
        nameField.text = values.name
        iconField.text = values.icon
        sourceBox.currentIndex = Math.max(0, sourceBox.indexOfValue(values.source))
        readBox.currentIndex = Math.max(0, readBox.indexOfValue(values.readState))
        sortBox.currentIndex = Math.max(0, sortBox.indexOfValue(values.sort))
        includeField.text = values.include
        excludeField.text = values.exclude
        picked = values.tags
        // The library's tags, and the view's own that no feed carries now.
        const offered = backend.viewTags()
        for (let i = 0; i < values.tags.length; ++i)
            if (offered.indexOf(values.tags[i]) < 0)
                offered.push(values.tags[i])
        tags = offered
        tagList.currentIndex = 0
        open()
    }

    function create() { load("", backend.viewForm("")) }
    function createFromSource() { load("", backend.sourceViewForm()) }
    function edit(id) { load(id, backend.viewForm(id)) }

    function toggleTag(tag) {
        const at = picked.indexOf(tag)
        picked = at < 0 ? picked.concat([tag]) : picked.filter(t => t !== tag)
    }

    function submit() {
        attempted = true
        if (problem === "" && backend.saveView(viewId, form))
            close()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 520)
    // The form scrolls in a window too short for it.
    height: Math.min(implicitHeight, parent.height - 32)
    modal: true
    title: viewId === "" ? qsTr("New view") : qsTr("Edit view")
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

    onOpened: nameField.forceActiveFocus()

    component FieldLabel: Label {
        textFormat: Text.PlainText
        font.pixelSize: 11
        opacity: 0.7
        elide: Text.ElideRight
        Layout.fillWidth: true
    }

    contentItem: Flickable {
        id: scroller
        implicitWidth: fields.implicitWidth
        implicitHeight: fields.implicitHeight
        contentHeight: fields.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        // Scrolls the field the keyboard moves to into view.
        function reveal(item) {
            let inside = item
            while (inside && inside !== fields)
                inside = inside.parent
            if (!inside)
                return
            const top = item.mapToItem(fields, 0, 0).y
            if (top < contentY)
                contentY = top
            else if (top + item.height > contentY + height)
                contentY = Math.min(top + item.height, contentHeight) - height
        }

        Connections {
            target: scroller.Window.window
            enabled: dialog.visible
            function onActiveFocusItemChanged() { scroller.reveal(scroller.Window.activeFocusItem) }
        }

        ColumnLayout {
            id: fields
            width: scroller.width
            spacing: 6

            // Room for the floating label of the name, which the scrolled
            // area would clip.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 8

                TextField {
                    id: nameField
                    objectName: "viewName"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Name")
                    maximumLength: backend.maxViewNameLength
                    Accessible.name: qsTr("Name")
                    Material.roundedScale: theme.radius
                    onAccepted: dialog.submit()
                }

                TextField {
                    id: iconField
                    objectName: "viewIcon"
                    Layout.preferredWidth: 96
                    placeholderText: qsTr("Icon")
                    maximumLength: backend.maxViewIconLength
                    Accessible.name: qsTr("Icon")
                    Material.roundedScale: theme.radius
                    onAccepted: dialog.submit()
                }
            }

            FieldLabel { text: qsTr("Lists") }

            ComboBox {
                id: sourceBox
                objectName: "viewSource"
                Layout.fillWidth: true
                visible: dialog.sourceEditable
                model: dialog.sources
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Lists")
                Material.roundedScale: theme.radius
            }

            Label {
                objectName: "viewFixedSource"
                Layout.fillWidth: true
                visible: !dialog.sourceEditable
                text: {
                    switch (dialog.fixedSource) {
                    case "feed": return qsTr("Feed (set on the web)")
                    case "category": return qsTr("Category (set on the web)")
                    default: return qsTr("Set on the web")
                    }
                }
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }

            // Up and Down move in the tags, and Space ticks the one at hand.
            ListView {
                id: tagList
                objectName: "viewTags"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Math.max(dialog.tags.length, 1), 4) * 36
                visible: dialog.sourceEditable && sourceBox.currentValue === "tags"
                clip: true
                model: dialog.tags
                activeFocusOnTab: true
                keyNavigationEnabled: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Accessible.name: qsTr("Tags")
                Keys.onSpacePressed: {
                    if (currentIndex >= 0 && currentIndex < dialog.tags.length)
                        dialog.toggleTag(dialog.tags[currentIndex])
                }

                delegate: CheckDelegate {
                    id: tagRow
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    height: 36
                    text: modelData
                    checkable: false
                    checked: dialog.picked.indexOf(modelData) >= 0
                    focusPolicy: Qt.NoFocus
                    highlighted: tagList.activeFocus && tagList.currentIndex === index
                    onClicked: {
                        tagList.currentIndex = index
                        dialog.toggleTag(modelData)
                    }
                    contentItem: Label {
                        text: "#" + tagRow.text
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        rightPadding: tagRow.indicator.width + tagRow.spacing
                    }
                }

                Label {
                    anchors.fill: parent
                    visible: dialog.tags.length === 0
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("No feed has a tag yet")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    opacity: 0.55
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 8
                rowSpacing: 0

                FieldLabel { text: qsTr("Articles") }
                FieldLabel { text: qsTr("Order") }

                ComboBox {
                    id: readBox
                    objectName: "viewReadState"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    model: dialog.readStates
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Articles")
                    Material.roundedScale: theme.radius
                }

                ComboBox {
                    id: sortBox
                    objectName: "viewSort"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    model: dialog.sorts
                    textRole: "text"
                    valueRole: "value"
                    Accessible.name: qsTr("Order")
                    Material.roundedScale: theme.radius
                }
            }

            TextField {
                id: includeField
                objectName: "viewInclude"
                Layout.fillWidth: true
                placeholderText: qsTr("Keywords, separated by commas")
                Accessible.name: qsTr("Keywords, separated by commas")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                Material.roundedScale: theme.radius
                onAccepted: dialog.submit()
            }

            TextField {
                id: excludeField
                objectName: "viewExclude"
                Layout.fillWidth: true
                placeholderText: qsTr("Mute words, separated by commas")
                Accessible.name: qsTr("Mute words, separated by commas")
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                Material.roundedScale: theme.radius
                onAccepted: dialog.submit()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Mute words stay on this computer and are never published.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 11
                opacity: 0.7
            }

            Label {
                objectName: "viewProblem"
                Layout.fillWidth: true
                visible: text !== ""
                text: dialog.attempted || nameField.text.trim() !== "" ? dialog.problem : ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                color: dialog.errorColor
            }
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
            objectName: "viewSave"
            text: dialog.viewId === "" ? qsTr("Create") : qsTr("Save")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.submit()
        }
    }
}

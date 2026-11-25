import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The keyword filter rules: a list to turn on and off, reorder, edit, and
// delete, and a form below it that adds a rule or saves the one being
// edited. Each change is saved at once. The rules stay on this machine.
Dialog {
    id: dialog
    objectName: "filtersDialog"

    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    readonly property var rules: backend.filterRules
    readonly property int rowHeight: 56
    // The id of the rule the form edits, or "" while it adds one, so that
    // the form keeps to that rule however the list changes.
    property string editingId: ""
    // Its row, or -1.
    readonly property int editing: indexOfRule(editingId)
    // An empty pattern is only an error once the user tried to save it.
    property bool attempted: false
    readonly property string problem: patternField.text === "" && !attempted ? ""
                                      : backend.filterProblem(typeBox.currentValue, patternField.text)
    readonly property bool full: editing < 0 && rules.length >= backend.maxFilterRules

    // A rule the form edits that is gone (the list read back after a save
    // failed) is no longer edited.
    onRulesChanged: {
        if (editingId !== "" && indexOfRule(editingId) < 0)
            resetForm()
    }

    readonly property var targets: [
        { value: "any", text: qsTr("Anywhere") },
        { value: "title", text: qsTr("Title") },
        { value: "content", text: qsTr("Text") },
        { value: "author", text: qsTr("Author") }
    ]
    readonly property var types: [
        { value: "contains", text: qsTr("Contains") },
        { value: "word", text: qsTr("Whole word") },
        { value: "regex", text: qsTr("Regular expression") }
    ]
    readonly property var actions: [
        { value: "hide", text: qsTr("Hide") },
        { value: "highlight", text: qsTr("Highlight") },
        { value: "mark-read", text: qsTr("Mark read") }
    ]

    function indexOfRule(id) {
        if (id === "")
            return -1
        for (let i = 0; i < rules.length; ++i)
            if (rules[i].id === id)
                return i
        return -1
    }

    function nameOf(list, value) {
        for (let i = 0; i < list.length; ++i)
            if (list[i].value === value)
                return list[i].text
        return value
    }

    function describe(rule) {
        const parts = [nameOf(actions, rule.action), nameOf(targets, rule.target), nameOf(types, rule.type)]
        if (rule.caseSensitive)
            parts.push(qsTr("Case sensitive"))
        return parts.join(" · ")
    }

    function resetForm() {
        editingId = ""
        attempted = false
        patternField.text = ""
        targetBox.currentIndex = 0
        typeBox.currentIndex = 0
        actionBox.currentIndex = 0
        caseBox.checked = false
    }

    function edit(index) {
        const rule = rules[index]
        editingId = rule.id
        attempted = false
        patternField.text = rule.pattern
        targetBox.currentIndex = targetBox.indexOfValue(rule.target)
        typeBox.currentIndex = typeBox.indexOfValue(rule.type)
        actionBox.currentIndex = actionBox.indexOfValue(rule.action)
        caseBox.checked = rule.caseSensitive
        patternField.forceActiveFocus()
    }

    // Whether the rule now marks articles read where before it did not, or
    // marks others, which the user may want applied to the library as well.
    function marksAnew(before, after) {
        if (after.action !== "mark-read" || !after.enabled)
            return false
        return !before || !before.enabled || before.action !== after.action || before.pattern !== after.pattern
            || before.target !== after.target || before.type !== after.type || before.caseSensitive !== after.caseSensitive
    }

    function submit() {
        attempted = true
        if (full || backend.filterProblem(typeBox.currentValue, patternField.text) !== "")
            return
        const index = indexOfRule(editingId)
        if (editingId !== "" && index < 0) {
            resetForm()
            return
        }
        const before = index >= 0 ? rules[index] : null
        const rule = {
            enabled: before ? before.enabled : true,
            target: targetBox.currentValue,
            type: typeBox.currentValue,
            pattern: patternField.text,
            caseSensitive: caseBox.checked,
            action: actionBox.currentValue
        }
        if (!backend.saveFilterRule(index, rule))
            return
        const saved = backend.filterRules[index >= 0 ? index : backend.filterRules.length - 1]
        resetForm()
        if (marksAnew(before, saved))
            applyDialog.ask(saved.id)
        else
            patternField.forceActiveFocus()
    }

    function setEnabled(index, enabled) {
        const before = rules[index]
        backend.setFilterRuleEnabled(index, enabled)
        const after = backend.filterRules[index]
        if (marksAnew(before, after))
            applyDialog.ask(after.id)
    }

    function remove(index) {
        if (editing === index)
            resetForm()
        backend.removeFilterRule(index)
    }

    function move(index, step) {
        const target = index + step
        if (target < 0 || target >= rules.length)
            return
        backend.moveFilterRule(index, step)
        list.positionViewAtIndex(target, ListView.Contain)
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 64, 640)
    modal: true
    title: qsTr("Filters")
    Material.roundedScale: theme.radius
    onOpened: {
        resetForm()
        patternField.forceActiveFocus()
    }

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

    // The window's Ctrl+Z and Ctrl+Shift+Z are off while the dialog is open,
    // so these undo and redo a rule change here, except while a text field
    // keeps them for its own typing or something covers the dialog.
    readonly property bool undoKeysFree: {
        const item = content.Window.activeFocusItem
        return opened && !applyDialog.visible && !targetBox.popup.visible && !typeBox.popup.visible
            && !actionBox.popup.visible && !(item instanceof TextInput) && !(item instanceof TextEdit)
    }
    Shortcut { objectName: "filterUndoShortcut"; sequences: ["Ctrl+Z"]; context: Qt.ApplicationShortcut; enabled: dialog.undoKeysFree; onActivated: backend.undo() }
    Shortcut { objectName: "filterRedoShortcut"; sequences: ["Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; enabled: dialog.undoKeysFree; onActivated: backend.redo() }

    // Enter presses a button as Space does.
    component KeyButton: Button {
        flat: true
        Material.roundedScale: theme.radius
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
    }

    contentItem: ColumnLayout {
        id: content
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: qsTr("Rules hide, highlight, or mark read the articles whose title, text, or author match. They stay on this computer and are never synced.")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            opacity: 0.7
        }

        Label {
            Layout.fillWidth: true
            visible: backend.filterError !== ""
            text: backend.filterError
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            color: dialog.errorColor
        }

        Label {
            Layout.fillWidth: true
            Layout.preferredHeight: dialog.rowHeight
            visible: dialog.rules.length === 0
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No rules yet")
            textFormat: Text.PlainText
            opacity: 0.55
        }

        ListView {
            id: list
            objectName: "filterList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(dialog.rules.length, 3) * dialog.rowHeight
            visible: dialog.rules.length > 0
            clip: true
            model: dialog.rules
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property int index
                required property var modelData

                width: ListView.view.width
                height: dialog.rowHeight

                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 2
                    anchors.bottomMargin: 2
                    radius: Math.min(4, theme.radius)
                    visible: dialog.editing === row.index
                    color: Qt.alpha(theme.accent, 0.16)
                }

                RowLayout {
                    anchors.fill: parent
                    spacing: 4

                    CheckBox {
                        objectName: "filterEnabled" + row.index
                        checked: row.modelData.enabled
                        Accessible.name: qsTr("Use this rule")
                        onToggled: dialog.setEnabled(row.index, checked)
                        Keys.onReturnPressed: { toggle(); toggled() }
                        Keys.onEnterPressed: { toggle(); toggled() }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        opacity: row.modelData.enabled ? 1 : 0.55

                        Label {
                            Layout.fillWidth: true
                            text: row.modelData.pattern
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.weight: Font.DemiBold
                        }

                        Label {
                            Layout.fillWidth: true
                            text: dialog.describe(row.modelData)
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.pixelSize: 11
                            opacity: 0.7
                        }
                    }

                    KeyButton {
                        objectName: "filterUp" + row.index
                        text: "↑"
                        enabled: row.index > 0
                        Accessible.name: qsTr("Move up")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Move up")
                        onClicked: dialog.move(row.index, -1)
                    }

                    KeyButton {
                        objectName: "filterDown" + row.index
                        text: "↓"
                        enabled: row.index < dialog.rules.length - 1
                        Accessible.name: qsTr("Move down")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Move down")
                        onClicked: dialog.move(row.index, 1)
                    }

                    KeyButton {
                        objectName: "filterEdit" + row.index
                        text: qsTr("Edit")
                        onClicked: dialog.edit(row.index)
                    }

                    KeyButton {
                        objectName: "filterDelete" + row.index
                        text: qsTr("Delete")
                        onClicked: dialog.remove(row.index)
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Qt.alpha(theme.foreground, 0.1) }

        TextField {
            id: patternField
            objectName: "filterPattern"
            Layout.fillWidth: true
            placeholderText: qsTr("Word, phrase, or pattern")
            maximumLength: backend.maxFilterPatternLength
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            Material.roundedScale: theme.radius
            onAccepted: dialog.submit()
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 8
            rowSpacing: 0

            Label { text: qsTr("Look in"); textFormat: Text.PlainText; font.pixelSize: 11; opacity: 0.7; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { text: qsTr("Match"); textFormat: Text.PlainText; font.pixelSize: 11; opacity: 0.7; Layout.fillWidth: true; elide: Text.ElideRight }
            Label { text: qsTr("Then"); textFormat: Text.PlainText; font.pixelSize: 11; opacity: 0.7; Layout.fillWidth: true; elide: Text.ElideRight }

            ComboBox {
                id: targetBox
                objectName: "filterTarget"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                model: dialog.targets
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Look in")
                Material.roundedScale: theme.radius
            }

            ComboBox {
                id: typeBox
                objectName: "filterType"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                model: dialog.types
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Match")
                Material.roundedScale: theme.radius
            }

            ComboBox {
                id: actionBox
                objectName: "filterAction"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                model: dialog.actions
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Then")
                Material.roundedScale: theme.radius
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            CheckBox {
                id: caseBox
                objectName: "filterCaseSensitive"
                text: qsTr("Case sensitive")
                Keys.onReturnPressed: toggle()
                Keys.onEnterPressed: toggle()
            }

            Item { Layout.fillWidth: true }

            KeyButton {
                objectName: "filterCancelEdit"
                visible: dialog.editing >= 0
                text: qsTr("Cancel editing")
                onClicked: {
                    dialog.resetForm()
                    patternField.forceActiveFocus()
                }
            }

            KeyButton {
                objectName: "filterSave"
                text: dialog.editing >= 0 ? qsTr("Save rule") : qsTr("Add rule")
                enabled: !dialog.full
                onClicked: dialog.submit()
            }
        }

        // RE2 lets a pattern turn case folding on for itself.
        Label {
            objectName: "filterCaseHint"
            Layout.fillWidth: true
            visible: typeBox.currentValue === "regex"
            text: qsTr("(?i) in the pattern ignores case even when Case sensitive is on")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: 11
            opacity: 0.7
        }

        Label {
            objectName: "filterProblem"
            Layout.fillWidth: true
            visible: text !== ""
            text: dialog.full ? qsTr("There can be at most %1 rules").arg(backend.maxFilterRules) : dialog.problem
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
            text: qsTr("Close")
            flat: true
            focusPolicy: Qt.NoFocus
            Material.roundedScale: theme.radius
            onClicked: dialog.close()
        }
    }

    // Asked once when a rule starts marking articles read: it marks new
    // ones on its own, and the library's only when the user says so.
    Dialog {
        id: applyDialog
        objectName: "filterApplyDialog"

        property string ruleId: ""

        function ask(id) {
            ruleId = id
            open()
        }

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 96, 460)
        modal: true
        title: qsTr("Mark matching articles read?")
        Material.roundedScale: theme.radius
        onOpened: markButton.forceActiveFocus()
        onClosed: patternField.forceActiveFocus()

        background: Rectangle {
            radius: theme.radius
            color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.08))
            border.color: Qt.alpha(theme.foreground, 0.15)
            border.width: 1
        }

        header: Item {
            implicitWidth: applyHeading.implicitWidth
            implicitHeight: applyHeading.implicitHeight

            Label {
                id: applyHeading
                width: parent.width
                text: applyDialog.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                padding: 24
                bottomPadding: 0
                font.pixelSize: 20
            }
        }

        contentItem: Label {
            text: qsTr("This rule marks new articles read as they arrive. Mark the articles already in the library that it matches read too?")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }

        footer: DialogButtonBox {
            background: null
            KeyButton {
                objectName: "filterApplyNo"
                text: qsTr("Not now")
                onClicked: applyDialog.close()
            }
            KeyButton {
                id: markButton
                objectName: "filterApplyYes"
                text: qsTr("Mark read")
                onClicked: {
                    backend.markFilterMatchesRead(applyDialog.ruleId)
                    applyDialog.close()
                }
            }
        }
    }
}

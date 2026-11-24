import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The articles of the selected source, newest first, or those of them a
// search finds, best match first.
Rectangle {
    id: pane

    property bool active: false
    // Whether Esc has a search to clear: one typed, or the field ready for one.
    readonly property bool searchOpen: searchField.activeFocus || searchField.text !== ""

    signal activated()

    color: theme.background

    function focusSearch() {
        searchField.forceActiveFocus()
        searchField.selectAll()
    }

    function clearSearch() {
        searchDelay.stop()
        searchField.clear()
        backend.searchQuery = ""
        leaveSearch()
    }

    // The keyboard goes back to the list, so J, K and the other single keys
    // act on it rather than type into the field.
    function leaveSearch() {
        if (searchField.activeFocus)
            list.forceActiveFocus()
    }

    function viewName() {
        switch (backend.sourceKind) {
        case "unread": return qsTr("Unread")
        case "starred": return qsTr("Starred")
        case "feed": return backend.sourceTitle
        default: return qsTr("All articles")
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.topMargin: 6
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            spacing: 0

            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: pane.viewName()
                textFormat: Text.PlainText
                elide: Text.ElideRight
                maximumLineCount: 1
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("%n articles", "", backend.articles.count)
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 11
                opacity: 0.55
            }
        }

        TextField {
            id: searchField
            objectName: "searchField"
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 6
            placeholderText: qsTr("Search articles")
            // As much as the library searches for; the rest would be ignored.
            maximumLength: 256
            inputMethodHints: Qt.ImhNoAutoUppercase
            Material.roundedScale: theme.radius
            // The list follows a pause in typing rather than every key, so a
            // large library is not searched once a letter.
            onTextEdited: searchDelay.restart()
            onAccepted: {
                searchDelay.stop()
                backend.searchQuery = text
                pane.leaveSearch()
            }
        }

        Timer {
            id: searchDelay
            interval: 250
            onTriggered: backend.searchQuery = searchField.text
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.articles
            currentIndex: backend.articleIndex
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property int index
                required property string feedTitle
                required property string title
                required property var published
                required property bool read
                required property bool starred

                readonly property bool selected: index === backend.articleIndex

                width: ListView.view.width
                height: content.implicitHeight + 20

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    radius: Math.min(4, theme.radius)
                    visible: row.selected || hover.hovered
                    color: row.selected ? Qt.alpha(theme.accent, pane.active ? 0.28 : 0.16) : Qt.alpha(theme.foreground, 0.06)
                }

                Rectangle {
                    visible: !row.read
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    y: 17
                    width: 7
                    height: 7
                    radius: 3.5
                    color: theme.accent
                }

                ColumnLayout {
                    id: content
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 26
                    anchors.rightMargin: 16
                    spacing: 4

                    Label {
                        Layout.fillWidth: true
                        text: row.title
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        font.weight: row.read ? Font.Normal : Font.DemiBold
                        opacity: row.read ? 0.7 : 1
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: row.feedTitle
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.pixelSize: 11
                            opacity: 0.55
                        }

                        Label {
                            text: {
                                translator.language
                                return row.published ? row.published.toLocaleDateString(Qt.locale(), Locale.ShortFormat) : ""
                            }
                            textFormat: Text.PlainText
                            font.pixelSize: 11
                            opacity: 0.55
                        }

                        Label {
                            visible: row.starred
                            text: "★"
                            textFormat: Text.PlainText
                            font.pixelSize: 12
                            color: theme.accent
                        }
                    }
                }

                HoverHandler { id: hover }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        pane.leaveSearch()
                        backend.selectArticle(row.index)
                        pane.activated()
                    }
                }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 48
        visible: backend.articles.count === 0 && backend.feedCount > 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: backend.searching ? qsTr("No articles match the search")
            : backend.sourceKind === "unread" ? qsTr("All caught up")
            : backend.sourceKind === "starred" ? qsTr("Press S to star an article")
            : qsTr("No articles yet")
        textFormat: Text.PlainText
        opacity: 0.55
    }
}

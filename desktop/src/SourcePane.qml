import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// All articles, Unread, and Starred, then the saved views synced from the
// web app, when there are any, then the feeds by category, with unread
// counts, a mark on Nostr authors, and the last refresh error of each feed
// (or, for a Nostr author, a quieter note when the refresh missed something).
// A view with rules that cannot be applied here says so, and one whose
// source cannot be told here is shown disabled.
Rectangle {
    id: pane

    // The tips name what a view holds, which comes from a relay, so they
    // are plain text: the default tip would take markup in it as rich text.
    component PlainToolTip: ToolTip {
        id: tip
        delay: 500
        contentItem: Text {
            text: tip.text
            textFormat: Text.PlainText
            font: tip.font
            wrapMode: Text.Wrap
            color: tip.Material.foreground
        }
    }

    property bool active: false
    readonly property bool menuOpen: appMenu.visible || feedMenu.visible
    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)

    // Qt Quick Controls menus take their width from the background alone and
    // elide longer labels, so each menu is as wide as its widest item, up to
    // half the window, which leaves room for the longest translations.
    function menuWidth(menu) {
        let widest = 0
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (item)
                widest = Math.max(widest, item.implicitWidth)
        }
        return Math.max(220, Math.min(widest + menu.leftPadding + menu.rightPadding, pane.Window.width / 2))
    }

    signal activated()
    signal addRequested()
    signal unsubscribeRequested(var feedId, string feedTitle)
    signal helpRequested()
    signal syncRequested()
    signal settingsRequested()
    signal filtersRequested()

    color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.03))

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            Layout.leftMargin: 14
            Layout.rightMargin: 4
            spacing: 0

            Label {
                Layout.fillWidth: true
                text: "Readstr"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            ToolButton {
                text: "+"
                font.pixelSize: 20
                focusPolicy: Qt.NoFocus
                Material.roundedScale: theme.radius
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add a feed (Ctrl+N)")
                onClicked: pane.addRequested()
            }

            ToolButton {
                text: "⋮"
                font.pixelSize: 18
                focusPolicy: Qt.NoFocus
                Material.roundedScale: theme.radius
                ToolTip.visible: hovered && !appMenu.visible
                ToolTip.text: qsTr("More")
                onClicked: appMenu.popup()

                Menu {
                    id: appMenu
                    objectName: "appMenu"
                    implicitWidth: pane.menuWidth(appMenu)
                    Material.roundedScale: theme.radius
                    background: Rectangle {
                        radius: Math.min(4, theme.radius)
                        color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.08))
                        border.color: Qt.alpha(theme.foreground, 0.15)
                        border.width: 1
                    }
                    MenuItem { text: qsTr("Refresh all feeds"); enabled: !backend.refreshing; onTriggered: backend.refreshAll() }
                    MenuItem { text: qsTr("Mark everything in the view read"); onTriggered: backend.markViewRead() }
                    MenuItem { text: qsTr("Undo"); enabled: backend.canUndo; onTriggered: backend.undo() }
                    MenuItem { text: qsTr("Redo"); enabled: backend.canRedo; onTriggered: backend.redo() }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Import OPML…"); onTriggered: backend.importOpml() }
                    MenuItem { text: qsTr("Export OPML…"); onTriggered: backend.exportOpml() }
                    MenuItem { text: qsTr("Sync with Nostr…"); onTriggered: pane.syncRequested() }
                    MenuItem { text: qsTr("Filters…"); onTriggered: pane.filtersRequested() }
                    MenuSeparator {}
                    MenuItem { text: qsTr("Settings…"); onTriggered: pane.settingsRequested() }
                    MenuItem { text: qsTr("Keyboard shortcuts"); onTriggered: pane.helpRequested() }
                }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backend.sources
            currentIndex: backend.sourceIndex
            keyNavigationEnabled: false
            boundsBehavior: Flickable.StopAtBounds
            bottomMargin: 8
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property int index
                required property string kind
                required property var feedId
                required property string title
                required property string category
                required property int unread
                required property string lastError
                required property bool selectable
                required property bool nostr
                required property bool incomplete
                required property string viewId
                required property string icon
                required property var unsupported
                required property bool unknownSource

                readonly property bool selected: index === backend.sourceIndex
                readonly property bool header: kind === "category" || kind === "views"

                width: ListView.view.width
                height: header ? 36 : (lastError !== "" || incomplete ? 50 : 34)

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 6
                    anchors.topMargin: 1
                    anchors.bottomMargin: 1
                    radius: Math.min(4, theme.radius)
                    visible: !row.header && (row.selected || (hover.hovered && row.selectable))
                    color: row.selected ? Qt.alpha(theme.accent, pane.active ? 0.28 : 0.16) : Qt.alpha(theme.foreground, 0.06)
                }

                Label {
                    visible: row.header
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    anchors.bottomMargin: 6
                    text: row.kind === "views" ? qsTr("Views")
                        : row.category === "" ? qsTr("Feeds") : row.category
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    opacity: 0.55
                }

                ColumnLayout {
                    visible: !row.header
                    opacity: row.unknownSource ? 0.45 : 1
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 14
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: row.kind === "all" ? qsTr("All articles")
                                : row.kind === "unread" ? qsTr("Unread")
                                : row.kind === "starred" ? qsTr("Starred")
                                : row.icon !== "" ? row.icon + " " + row.title
                                : row.title
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            // A title with a line break must not grow the row.
                            maximumLineCount: 1
                            font.weight: row.kind === "feed" || row.kind === "view" ? Font.Normal : Font.DemiBold
                        }

                        // The view lists more here than in the web app:
                        // which of its rules are left out is in the tip.
                        Label {
                            objectName: "viewUnsupported"
                            visible: row.unsupported.length > 0 && !row.unknownSource
                            text: qsTr("Partial")
                            textFormat: Text.PlainText
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            opacity: 0.5

                            HoverHandler { id: unsupportedHover }
                            PlainToolTip {
                                visible: unsupportedHover.hovered
                                text: qsTr("Some rules of this view are not supported here, so they are left out: %1").arg(row.unsupported.join(", "))
                            }
                        }

                        Label {
                            visible: row.nostr
                            text: qsTr("Nostr")
                            textFormat: Text.PlainText
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            opacity: 0.5
                            ToolTip.visible: nostrHover.hovered
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("A Nostr author's articles and videos")

                            HoverHandler { id: nostrHover }
                        }

                        Rectangle {
                            visible: row.lastError !== ""
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: pane.errorColor
                        }

                        Rectangle {
                            visible: row.unread > 0
                            Layout.preferredHeight: 20
                            Layout.preferredWidth: Math.max(20, countLabel.implicitWidth + 12)
                            radius: Math.min(10, theme.radius)
                            color: row.kind === "starred" ? Qt.alpha(theme.foreground, 0.12) : Qt.alpha(theme.accent, 0.22)

                            Label {
                                id: countLabel
                                anchors.centerIn: parent
                                text: {
                                    translator.language
                                    return row.unread.toLocaleString(Qt.locale(), 'f', 0)
                                }
                                textFormat: Text.PlainText
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                        }
                    }

                    Label {
                        visible: row.lastError !== ""
                        Layout.fillWidth: true
                        text: row.lastError
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        font.pixelSize: 11
                        color: pane.errorColor
                    }

                    // Not an error: the refresh worked, but a relay was
                    // silent or a limit cut the answers short.
                    Label {
                        visible: row.incomplete
                        Layout.fillWidth: true
                        text: qsTr("Refresh incomplete")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        font.pixelSize: 11
                        opacity: 0.6
                        ToolTip.visible: incompleteHover.hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("The last refresh did not hear from every relay, or stopped at a limit, so some articles may be missing. The next refresh asks for them again.")

                        HoverHandler { id: incompleteHover }
                    }
                }

                HoverHandler { id: hover }

                PlainToolTip {
                    objectName: "viewUnknownSource"
                    visible: row.unknownSource && hover.hovered
                    text: qsTr("This view targets a feed, category, or tags that this app can't identify yet")
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: row.selectable
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        backend.selectSource(row.index)
                        pane.activated()
                        if (mouse.button === Qt.RightButton && row.kind === "feed") {
                            feedMenu.feedId = row.feedId
                            feedMenu.feedTitle = row.title
                            feedMenu.nostr = row.nostr
                            feedMenu.popup(row, mouse.x, mouse.y)
                        }
                    }
                }
            }
        }
    }

    Menu {
        id: feedMenu
        property var feedId: 0
        property string feedTitle: ""
        property bool nostr: false
        objectName: "feedMenu"
        implicitWidth: pane.menuWidth(feedMenu)
        Material.roundedScale: theme.radius
        background: Rectangle {
            radius: Math.min(4, theme.radius)
            color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.08))
            border.color: Qt.alpha(theme.foreground, 0.15)
            border.width: 1
        }
        MenuItem { text: qsTr("Refresh"); enabled: !backend.refreshing; onTriggered: backend.refreshFeed(feedMenu.feedId) }
        // A hidden item still takes its place in a Menu unless its height goes too.
        MenuItem { text: qsTr("Open profile"); visible: feedMenu.nostr; height: visible ? implicitHeight : 0; onTriggered: backend.openProfile(feedMenu.feedId) }
        MenuItem { text: qsTr("Copy npub"); visible: feedMenu.nostr; height: visible ? implicitHeight : 0; onTriggered: backend.copyNpub(feedMenu.feedId) }
        MenuItem { text: qsTr("Unsubscribe…"); onTriggered: pane.unsubscribeRequested(feedMenu.feedId, feedMenu.feedTitle) }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1200
    height: 760
    minimumWidth: 760
    minimumHeight: 460
    visible: true
    title: "Readstr"
    color: theme.background

    // Light Omarchy themes need Material's light palette for hint text, ripples and popups.
    Material.theme: Qt.color(theme.background).hslLightness > 0.5 ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    // The list Up and Down move in: "sources" or "articles".
    property string activePane: "articles"
    readonly property color errorColor: Material.color(Material.Red, Material.theme === Material.Light ? Material.Shade700 : Material.Shade300)
    // Single keys must not fire while a dialog or menu has the keyboard, or
    // typing an address would star articles.
    readonly property bool dialogsClosed: !addDialog.visible && !unsubscribeDialog.visible && !syncDialog.visible
                                          && !settingsDialog.visible && !filtersDialog.visible && !sourcePane.menuOpen
    // Nor behind the shortcuts overlay, which only ? and Esc close.
    readonly property bool keysFree: dialogsClosed && !helpVisible
    // A text field keeps Ctrl+Z and Ctrl+Shift+Z for undoing its own typing.
    readonly property bool editingText: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit

    // Re-evaluated on a language switch, like every other qsTr() binding.
    readonly property var shortcuts: [
        { keys: "J, K", action: qsTr("Next or previous article") },
        { keys: "N", action: qsTr("Next unread article") },
        { keys: "Up, Down", action: qsTr("Move in the selected list") },
        { keys: "Tab", action: qsTr("Switch between sources and articles") },
        { keys: "Space, Shift+Space", action: qsTr("Scroll the article") },
        { keys: "M", action: qsTr("Mark read or unread") },
        { keys: "S", action: qsTr("Star or unstar") },
        { keys: "O", action: qsTr("Open the original in the browser") },
        { keys: "P", action: qsTr("Play the video in mpv") },
        { keys: "I", action: qsTr("Load images for this article") },
        { keys: "Ctrl+R", action: qsTr("Refresh all feeds") },
        { keys: "Ctrl+N", action: qsTr("Add a feed") },
        { keys: "Ctrl+Shift+A", action: qsTr("Mark everything in the view read") },
        { keys: "Ctrl+Z", action: qsTr("Undo the last change") },
        { keys: "Ctrl+Shift+Z", action: qsTr("Redo what was undone") },
        { keys: "Ctrl+O", action: qsTr("Import OPML") },
        { keys: "Ctrl+Shift+E", action: qsTr("Export OPML") },
        { keys: "Ctrl+C", action: qsTr("Copy the selected text") },
        { keys: "Delete", action: qsTr("Unsubscribe from the selected feed") },
        { keys: "?", action: qsTr("Show or hide the shortcuts") },
        { keys: "Ctrl+F", action: qsTr("Search the articles") },
        { keys: "Esc", action: qsTr("Close the shortcuts, or clear the search") },
        { keys: "Q, Ctrl+Q", action: qsTr("Quit") }
    ]

    function askUnsubscribe(feedId, feedTitle) {
        if (feedId > 0)
            unsubscribeDialog.ask(feedId, feedTitle)
    }

    function move(step) {
        if (activePane === "sources")
            backend.moveSource(step)
        else
            backend.moveArticle(step)
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: win.dialogsClosed; onActivated: win.helpVisible = !win.helpVisible }
    // Esc closes the overlay first, which hides the search, and clears the
    // search only once nothing covers it; a dialog's Esc is its own.
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible && win.dialogsClosed; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.keysFree && articleList.searchOpen; onActivated: articleList.clearSearch() }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: { win.activePane = "articles"; articleList.focusSearch() } }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: Qt.quit() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut { sequence: "J"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: { win.activePane = "articles"; backend.moveArticle(1) } }
    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: { win.activePane = "articles"; backend.moveArticle(-1) } }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: { win.activePane = "articles"; backend.nextUnread() } }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.move(1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.move(-1) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.activePane = win.activePane === "sources" ? "articles" : "sources" }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: reader.scrollPage(1) }
    Shortcut { sequence: "Shift+Space"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: reader.scrollPage(-1) }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.toggleRead() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.toggleStar() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.openOriginal() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.playVideo() }
    // The button above the article takes no focus, so this is how the keyboard loads its images.
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.keysFree && backend.articleHasImages && !backend.articleImagesShown; onActivated: backend.loadArticleImages() }
    Shortcut { sequence: "Ctrl+R"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.refreshAll() }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: addDialog.open() }
    Shortcut { sequence: "Ctrl+Shift+A"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.markViewRead() }
    Shortcut { objectName: "undoShortcut"; sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: win.keysFree && !win.editingText; onActivated: backend.undo() }
    // Where StandardKey.Redo is Ctrl+Shift+Z already, as on Linux, the key
    // is bound twice here and Qt calls it ambiguous, though it is one action.
    Shortcut { objectName: "redoShortcut"; sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; enabled: win.keysFree && !win.editingText; onActivated: backend.redo(); onActivatedAmbiguously: backend.redo() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.importOpml() }
    Shortcut { sequence: "Ctrl+Shift+E"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: backend.exportOpml() }
    // The article never takes the keyboard, so its selection is copied
    // from here, without the zero-width spaces that break long words.
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: win.keysFree && reader.selectedText !== ""; onActivated: backend.copyText(reader.selectedText) }
    Shortcut { sequence: "Delete"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.askUnsubscribe(backend.selectedFeedId, backend.sourceTitle) }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                SourcePane {
                    id: sourcePane
                    Layout.preferredWidth: Math.max(200, Math.min(280, win.width * 0.2))
                    Layout.fillHeight: true
                    active: win.activePane === "sources"
                    onActivated: win.activePane = "sources"
                    onAddRequested: addDialog.open()
                    onUnsubscribeRequested: (feedId, feedTitle) => win.askUnsubscribe(feedId, feedTitle)
                    onHelpRequested: win.helpVisible = true
                    onSyncRequested: syncDialog.open()
                    onSettingsRequested: settingsDialog.open()
                    onFiltersRequested: filtersDialog.open()
                }

                Rectangle { Layout.fillHeight: true; Layout.preferredWidth: 1; color: Qt.alpha(theme.foreground, 0.1) }

                ArticleList {
                    id: articleList
                    Layout.preferredWidth: Math.max(260, Math.min(420, win.width * 0.3))
                    Layout.fillHeight: true
                    active: win.activePane === "articles"
                    onActivated: win.activePane = "articles"
                }

                Rectangle { Layout.fillHeight: true; Layout.preferredWidth: 1; color: Qt.alpha(theme.foreground, 0.1) }

                ReaderPane {
                    id: reader
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onAddRequested: addDialog.open()
                }
            }

            // Says what is going on and what went wrong.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.04))

                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.1) }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        text: backend.status
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: backend.statusError ? win.errorColor : theme.foreground
                        opacity: backend.statusError ? 1 : 0.7
                        font.pixelSize: 12
                    }

                    Label {
                        visible: backend.refreshing
                        textFormat: Text.PlainText
                        text: {
                            translator.language
                            return qsTr("Refreshing %1 of %2")
                                .arg(backend.refreshDone.toLocaleString(Qt.locale(), 'f', 0))
                                .arg(backend.refreshTotal.toLocaleString(Qt.locale(), 'f', 0))
                        }
                        opacity: 0.7
                        font.pixelSize: 12
                    }

                    ProgressBar {
                        visible: backend.refreshing
                        Layout.preferredWidth: 120
                        from: 0
                        to: Math.max(1, backend.refreshTotal)
                        value: backend.refreshDone
                    }
                }
            }
        }

        ShortcutsOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            shortcuts: win.shortcuts
            onCloseRequested: win.helpVisible = false
        }
    }

    AddFeedDialog { id: addDialog }
    UnsubscribeDialog { id: unsubscribeDialog }
    SyncDialog { id: syncDialog }
    SettingsDialog { id: settingsDialog }
    FiltersDialog { id: filtersDialog }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The selected article. Its body is the sanitizer's output, the only rich
// text in the window; every other string from a feed is plain text.
Rectangle {
    id: pane

    signal addRequested()

    color: theme.background

    function scrollPage(direction) {
        const page = Math.max(40, flick.height - 60)
        const bottom = Math.max(0, flick.contentHeight - flick.height)
        flick.contentY = Math.max(0, Math.min(bottom, flick.contentY + direction * page))
    }

    // A new article starts at its top.
    property var shownId: backend.articleId
    onShownIdChanged: flick.contentY = 0

    Flickable {
        id: flick
        anchors.fill: parent
        visible: backend.hasArticle
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + 72
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: column
            width: Math.min(flick.width - 64, 760)
            x: (flick.width - width) / 2
            y: 36
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: backend.articleTitle
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.pixelSize: 26
                font.weight: Font.DemiBold
            }

            Flow {
                Layout.fillWidth: true
                spacing: 14

                Label {
                    text: backend.articleFeed
                    textFormat: Text.PlainText
                    color: theme.accent
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    width: Math.min(implicitWidth, column.width)
                }

                Label {
                    visible: backend.articleAuthor !== ""
                    text: qsTr("by %1").arg(backend.articleAuthor)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    width: Math.min(implicitWidth, column.width)
                    opacity: 0.7
                }

                Label {
                    visible: text !== ""
                    text: {
                        translator.language
                        const date = backend.articlePublished
                        return date ? date.toLocaleDateString(Qt.locale(), Locale.LongFormat) + "  "
                                      + date.toLocaleTimeString(Qt.locale(), Locale.ShortFormat) : ""
                    }
                    textFormat: Text.PlainText
                    opacity: 0.7
                }

                Label {
                    visible: backend.articleStarred
                    text: "★ " + qsTr("Starred")
                    textFormat: Text.PlainText
                    color: theme.accent
                }
            }

            Label {
                id: original
                visible: backend.articleUrl !== ""
                Layout.fillWidth: true
                text: backend.articleUrl
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                font.pixelSize: 12
                font.underline: originalHover.hovered
                color: theme.accent
                opacity: 0.85

                HoverHandler { id: originalHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: backend.openOriginal() }
                ToolTip.visible: originalHover.hovered
                ToolTip.text: qsTr("Open the original in the browser (O)")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                Layout.bottomMargin: 6
                Layout.preferredHeight: 1
                color: Qt.alpha(theme.foreground, 0.12)
            }

            TextEdit {
                id: body
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                // Never takes the keyboard, so J, K, and Space keep working.
                activeFocusOnPress: false
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.RichText
                color: theme.foreground
                selectionColor: Qt.alpha(theme.accent, 0.4)
                selectedTextColor: theme.foreground
                font.pixelSize: 16
                // The style is ours; the markup after it is the sanitizer's.
                text: "<style>a { color: " + theme.accent + "; } pre, code { font-family: monospace; }"
                      + " th, td { padding: 4px 16px 4px 0; } th { text-align: left; }</style>"
                      + backend.articleHtml
                onLinkActivated: (link) => backend.openLink(link)

                HoverHandler {
                    id: bodyHover
                    cursorShape: body.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.IBeamCursor
                }

                // Where a link really goes, as plain text, before it is
                // clicked: a link's words can claim anything.
                ToolTip {
                    id: linkTip
                    visible: body.hoveredLink !== ""
                    delay: 300
                    // Follows the hover alone; closing on Esc or a click would
                    // break the binding above for good.
                    closePolicy: Popup.NoAutoClose
                    x: Math.min(Math.max(0, bodyHover.point.position.x), Math.max(0, body.width - width))
                    y: bodyHover.point.position.y + 24
                    width: Math.min(implicitWidth, column.width)
                    contentItem: Label {
                        text: body.hoveredLink
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        maximumLineCount: 1
                        font.pixelSize: 12
                        color: linkTip.Material.foreground
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 420)
        visible: !backend.hasArticle
        spacing: 12

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: backend.feedCount === 0 ? qsTr("No feeds yet") : qsTr("Pick an article to read it")
            font.pixelSize: backend.feedCount === 0 ? 24 : 16
            font.weight: Font.DemiBold
            opacity: backend.feedCount === 0 ? 1 : 0.55
        }

        Label {
            Layout.fillWidth: true
            visible: backend.feedCount === 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: qsTr("Add a feed with Ctrl+N or import your subscriptions with Ctrl+O. Press ? to see the keyboard shortcuts.")
            opacity: 0.6
        }

        Button {
            id: addButton
            Layout.alignment: Qt.AlignHCenter
            visible: backend.feedCount === 0
            text: qsTr("Add a feed")
            focusPolicy: Qt.NoFocus
            leftPadding: 20
            rightPadding: 20
            // Plain colors rather than Material's elevation layer, which the
            // software scene graph leaves out.
            background: Rectangle {
                implicitHeight: 40
                radius: Math.min(8, theme.radius)
                color: addButton.down ? Qt.darker(theme.accent, 1.15) : addButton.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            }
            contentItem: Label {
                text: addButton.text
                textFormat: Text.PlainText
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.weight: Font.DemiBold
                color: theme.accentForeground
            }
            onClicked: pane.addRequested()
        }
    }
}

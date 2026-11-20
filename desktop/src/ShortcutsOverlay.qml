import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, in two columns of pairs when the window is wide enough.
Rectangle {
    id: overlay

    property var shortcuts: []
    readonly property bool wide: width > 900
    readonly property int half: Math.ceil(shortcuts.length / 2)

    signal closeRequested()

    color: "#b3000000"

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closeRequested()
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, helpColumn.implicitWidth + 48)
        height: Math.min(parent.height - 64, helpColumn.implicitHeight + 48)
        radius: theme.radius
        color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.06))
        border.color: theme.accent
        border.width: 1
        clip: true

        // Clicks on the panel itself should not close the overlay.
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: helpColumn
            anchors.fill: parent
            anchors.margins: 24
            spacing: 16

            Label {
                text: qsTr("Keyboard shortcuts")
                textFormat: Text.PlainText
                font.pixelSize: 18
                font.weight: Font.DemiBold
                color: theme.accent
            }

            GridLayout {
                columns: overlay.wide ? 4 : 2
                columnSpacing: 24
                rowSpacing: 8

                Repeater {
                    model: overlay.shortcuts

                    delegate: Label {
                        required property var modelData
                        required property int index
                        readonly property bool second: overlay.wide && index >= overlay.half
                        text: modelData.keys
                        textFormat: Text.PlainText
                        font.family: "monospace"
                        font.weight: Font.DemiBold
                        Layout.row: second ? index - overlay.half : index
                        Layout.column: second ? 2 : 0
                    }
                }

                Repeater {
                    model: overlay.shortcuts

                    delegate: Label {
                        required property var modelData
                        required property int index
                        readonly property bool second: overlay.wide && index >= overlay.half
                        text: modelData.action
                        textFormat: Text.PlainText
                        opacity: 0.85
                        Layout.row: second ? index - overlay.half : index
                        Layout.column: second ? 3 : 1
                        Layout.fillWidth: true
                    }
                }
            }
        }
    }
}

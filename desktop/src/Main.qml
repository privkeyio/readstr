import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 640
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

    // Re-evaluated on a language switch, like every other qsTr() binding.
    readonly property var shortcuts: [
        { keys: "?", action: qsTr("Show or hide the shortcuts") },
        { keys: "Esc", action: qsTr("Close the shortcuts") },
        { keys: "Q, Ctrl+Q", action: qsTr("Quit") }
    ]

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequences: ["Q", "Ctrl+Q"]; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 64, 480)
            spacing: 12

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("No feeds yet")
                font.pixelSize: 24
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Press ? to see the keyboard shortcuts")
                opacity: 0.6
            }
        }

        Rectangle {
            id: helpOverlay
            anchors.fill: parent
            visible: win.helpVisible
            color: "#b3000000"

            MouseArea {
                anchors.fill: parent
                onClicked: win.helpVisible = false
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, helpColumn.implicitWidth + 48)
                height: helpColumn.implicitHeight + 48
                radius: theme.radius
                color: Qt.tint(theme.background, Qt.alpha(theme.foreground, 0.06))
                border.color: theme.accent
                border.width: 1

                // Clicks on the panel itself should not close the overlay.
                MouseArea { anchors.fill: parent }

                ColumnLayout {
                    id: helpColumn
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 16

                    Label {
                        text: qsTr("Keyboard shortcuts")
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                        color: theme.accent
                    }

                    GridLayout {
                        columns: 2
                        columnSpacing: 24
                        rowSpacing: 10

                        Repeater {
                            model: win.shortcuts

                            delegate: Label {
                                required property var modelData
                                required property int index
                                text: modelData.keys
                                font.family: "monospace"
                                font.weight: Font.DemiBold
                                Layout.row: index
                                Layout.column: 0
                            }
                        }

                        Repeater {
                            model: win.shortcuts

                            delegate: Label {
                                required property var modelData
                                required property int index
                                text: modelData.action
                                Layout.row: index
                                Layout.column: 1
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }
}

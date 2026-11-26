import QtQuick

// Asks before a feed and its articles leave the library.
ConfirmDialog {
    id: dialog

    property var feedId: 0
    property string feedTitle: ""

    function ask(id, name) {
        feedId = id
        feedTitle = name
        open()
    }

    title: qsTr("Unsubscribe?")
    message: qsTr("%1 and its articles will be removed from the library.").arg(feedTitle)
    confirmText: qsTr("Unsubscribe")
    onConfirmed: backend.unsubscribe(feedId)
}

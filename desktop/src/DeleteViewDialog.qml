import QtQuick

// Asks before a saved view goes. Deletions do not sync, so the web app
// keeps its copy until it is deleted there too.
ConfirmDialog {
    id: dialog
    objectName: "deleteViewDialog"

    property string viewId: ""
    property string viewName: ""

    function ask(id, name) {
        if (id === "")
            return
        viewId = id
        viewName = name
        open()
    }

    title: qsTr("Delete the view?")
    message: qsTr("%1 will be removed from the views here. The web app keeps its own copy until you delete it there too.").arg(viewName)
    confirmText: qsTr("Delete")
    confirmObjectName: "deleteViewConfirm"
    onConfirmed: backend.deleteView(viewId)
}

import QtQuick
import QtQuick.Controls

// Short messages at the bottom of the window. kind: info, error, undo (offers an Undo button).
Item {
    id: root
    signal undoClicked
    width: 420
    height: stack.height

    function show(text, kind) {
        // an identical message replaces the old one instead of stacking
        for (let i = 0; i < list.count; ++i) {
            if (list.get(i).text === text) {
                list.remove(i)
                break
            }
        }
        if (list.count >= 3)
            list.remove(0)
        list.append({ text: text, kind: kind || "info" })
    }

    ListModel { id: list }

    Column {
        id: stack
        width: parent.width
        spacing: 6
        Repeater {
            model: list
            delegate: Rectangle {
                id: toast
                width: stack.width
                height: row.implicitHeight + 16
                radius: 8
                color: "#1b1e25"
                border.color: model.kind === "error" ? Theme.danger : Theme.lineStrong
                Row {
                    id: row
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10
                    Text {
                        width: parent.width - (undoBtn.visible ? undoBtn.width + 10 : 0)
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.text
                        wrapMode: Text.Wrap
                        color: model.kind === "error" ? Theme.danger : Theme.text
                        font.pixelSize: 12
                    }
                    Btn {
                        id: undoBtn
                        visible: model.kind === "undo"
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Undo"
                        onClicked: {
                            root.undoClicked()
                            list.remove(index)
                        }
                    }
                }
                Timer {
                    interval: Math.max(model.kind === "info" ? 3000 : 6500, model.text.length * 45)
                    running: true
                    onTriggered: list.remove(index)
                }
            }
        }
    }
}

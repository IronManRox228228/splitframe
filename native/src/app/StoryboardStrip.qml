import QtQuick
import QtQuick.Layouts

// A compact storyboard for the Assistant (storyboard.get): one line per scene with its status dot and length. Collapsible; hidden
// while the project has no storyboard. `scenes` is [{id, title, intent, durationSec, status, ...}].
Rectangle {
    id: strip
    property var scenes: []
    property bool expanded: true

    visible: scenes.length > 0
    radius: 12
    color: Theme.cardFill
    border.width: 1
    border.color: Theme.line
    implicitHeight: col.implicitHeight

    function statusColor(s) {
        switch (String(s)) {
        case "done": case "ready": case "generated": case "approved": return Theme.success
        case "generating": case "rendering": case "running": return Theme.accent
        case "failed": case "error": return Theme.danger
        }
        return Theme.faint
    }

    ColumnLayout {
        id: col
        width: parent.width
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 32
            color: headArea.containsMouse ? "#0fffffff" : "transparent"
            radius: 12
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8
                Icon { name: "film"; size: 14; color: Theme.accent }
                Text { text: "Storyboard"; color: Theme.fg2; font.pixelSize: 12; font.weight: Font.Medium }
                Text { text: strip.scenes.length + (strip.scenes.length === 1 ? " scene" : " scenes"); color: Theme.muted; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Icon { name: strip.expanded ? "chevronDown" : "chevronRight"; size: 13; color: Theme.faint }
            }
            MouseArea { id: headArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: strip.expanded = !strip.expanded }
        }
        Rectangle { visible: strip.expanded; Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
        ColumnLayout {
            visible: strip.expanded
            Layout.fillWidth: true
            Layout.margins: 10
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            spacing: 6
            Repeater {
                model: strip.scenes
                delegate: RowLayout {
                    id: scene
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 5; implicitWidth: 7; implicitHeight: 7; radius: 3.5; color: strip.statusColor(scene.modelData.status) }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: (scene.index + 1) + ". " + (scene.modelData.title || scene.modelData.id)
                            color: Theme.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: text !== ""
                            Layout.fillWidth: true
                            text: scene.modelData.intent || ""
                            color: Theme.muted
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                    Text {
                        Layout.alignment: Qt.AlignTop
                        text: Number(scene.modelData.durationSec || 0).toFixed(1) + " s"
                        color: Theme.faint
                        font.family: Theme.mono
                        font.pixelSize: 11
                    }
                }
            }
        }
    }
}

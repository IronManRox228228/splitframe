import QtQuick
import QtQuick.Controls

// Port of the icon rail in editor/LeftPanel.tsx: a 76px glass column of labelled icons that opens one panel at a time; clicking the
// open one collapses it. The native editor has two panels, Media and Assistant (Text, Audio and Captions of the Electron rail live in
// the inspector, the Audio page and the crew).
GlassPanel {
    id: rail
    property string current: "media" // "" = collapsed
    signal picked(string id)

    readonly property var entries: [
        { id: "media", label: "Media", icon: "media" },
        { id: "assistant", label: "Assistant", icon: "sparkles" }
    ]

    implicitWidth: 76
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 10
        spacing: 4
        Repeater {
            model: rail.entries
            delegate: Rectangle {
                id: item
                required property var modelData
                readonly property bool active: rail.current === modelData.id
                width: 64
                height: 54
                radius: 16
                color: active ? "#1aeceeec" : (area.containsMouse ? "#0fffffff" : "transparent")
                Column {
                    anchors.centerIn: parent
                    spacing: 4
                    Icon { anchors.horizontalCenter: parent.horizontalCenter; name: item.modelData.icon; size: 20; color: item.active || area.containsMouse ? Theme.text : Theme.muted }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: item.modelData.label; font.pixelSize: 11; color: item.active || area.containsMouse ? Theme.text : Theme.muted }
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: rail.picked(item.active ? "" : item.modelData.id)
                }
                ToolTip.visible: area.containsMouse
                ToolTip.delay: 600
                ToolTip.text: active ? "Hide " + modelData.label : modelData.label
            }
        }
    }
}

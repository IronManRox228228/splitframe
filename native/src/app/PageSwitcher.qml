import QtQuick

// The segmented Edit / Colour / Audio switch in the top bar.
Rectangle {
    id: root
    property int current: 0
    property var pages: ["Edit", "Colour", "Audio"]
    signal picked(int index)

    implicitHeight: 32
    implicitWidth: row.implicitWidth + 8
    radius: Theme.pill
    color: Theme.glassFill
    border.width: 1
    border.color: Theme.glassBorder

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            model: root.pages
            delegate: Rectangle {
                id: seg
                required property int index
                required property string modelData
                width: 76
                height: 24
                radius: Theme.pill
                color: root.current === index ? Theme.text : (area.containsMouse ? "#14ffffff" : "transparent")
                Text {
                    anchors.centerIn: parent
                    text: seg.modelData
                    font.pixelSize: 12
                    font.weight: root.current === seg.index ? Font.DemiBold : Font.Normal
                    color: root.current === seg.index ? "#0B0D0C" : Theme.fg2
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.picked(seg.index)
                }
            }
        }
    }
}

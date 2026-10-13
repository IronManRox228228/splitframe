import QtQuick

// A vertical gain fader in dB (linear in dB between `minDb` and `maxDb`). Double-click returns to 0 dB.
Item {
    id: root
    property real db: 0
    property real minDb: -60
    property real maxDb: 12
    property bool muted: false
    signal moved(real db)
    signal released

    implicitWidth: 44
    implicitHeight: 200

    readonly property real travel: height - thumb.height
    function yFor(v) { return (1 - (Math.max(minDb, Math.min(maxDb, v)) - minDb) / (maxDb - minDb)) * travel }
    function dbAt(y) {
        const t = 1 - Math.max(0, Math.min(travel, y - thumb.height / 2)) / travel
        return minDb + t * (maxDb - minDb)
    }

    // scale ticks
    Repeater {
        model: [12, 6, 0, -6, -12, -24, -36, -48, -60]
        delegate: Item {
            required property int modelData
            y: root.yFor(modelData) + thumb.height / 2
            width: root.width
            Text {
                x: 0
                anchors.verticalCenter: parent.verticalCenter
                text: modelData === -60 ? "-\u221E" : modelData
                color: modelData === 0 ? Theme.fg2 : Theme.faint
                font.pixelSize: 9
                font.family: Theme.mono
            }
        }
    }

    // groove
    Rectangle {
        x: root.width - 16
        y: thumb.height / 2
        width: 4
        height: root.travel
        radius: 2
        color: Theme.line
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.height - root.yFor(root.db)
            radius: 2
            color: root.muted ? Theme.faint : Theme.accentDeep
        }
    }

    Rectangle {
        id: thumb
        x: root.width - 28
        y: root.yFor(root.db)
        width: 28
        height: 16
        radius: 5
        color: area.pressed ? Theme.accentHover : (area.containsMouse ? Theme.text : Theme.fg2)
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 8
            height: 2
            radius: 1
            color: "#0B0D0C"
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onPressed: mouse => root.moved(root.dbAt(mouse.y))
        onPositionChanged: mouse => { if (pressed) root.moved(root.dbAt(mouse.y)) }
        onReleased: root.released()
        onDoubleClicked: {
            root.moved(0)
            root.released()
        }
    }
}

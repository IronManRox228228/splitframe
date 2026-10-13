import QtQuick

// Stereo peak meter in dB (floor -60): a bar per channel, green to amber to red, and a falling peak-hold tick.
Item {
    id: root
    property real peakL: -60
    property real peakR: -60
    property real holdL: -60
    property real holdR: -60
    property real floorDb: -60
    property real topDb: 6

    implicitWidth: 14
    implicitHeight: 200

    function frac(db) { return Math.max(0, Math.min(1, (db - floorDb) / (topDb - floorDb))) }

    component Bar: Item {
        id: bar
        property real peak: -60
        property real hold: -60
        width: 6
        height: root.height
        Rectangle { anchors.fill: parent; radius: 3; color: Theme.field; border.width: 1; border.color: Theme.line }
        Item {
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            Rectangle {
                width: parent.width
                height: parent.height * root.frac(bar.peak)
                anchors.bottom: parent.bottom
                gradient: Gradient {
                    // the gradient spans the filled part, so colour stops follow the dB scale of the full bar
                    GradientStop { position: 0.0; color: bar.peak > -3 ? Theme.danger : (bar.peak > -12 ? Theme.warn : Theme.success) }
                    GradientStop { position: 1.0; color: Theme.success }
                }
                Behavior on height { NumberAnimation { duration: 50 } }
            }
            Rectangle {
                visible: bar.hold > root.floorDb
                width: parent.width
                height: 2
                y: parent.height * (1 - root.frac(bar.hold)) - 1
                color: bar.hold > -3 ? Theme.danger : Theme.fg2
            }
        }
    }

    Row {
        spacing: 2
        Bar { peak: root.peakL; hold: root.holdL }
        Bar { peak: root.peakR; hold: root.holdR }
    }
}

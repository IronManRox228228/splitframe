import QtQuick

// The agent that produced a card: a small dot and its name, in the crew colour (Director, Editor, Colourist, Audio).
Row {
    id: chip
    property string agent: ""
    property int pixelSize: 11
    spacing: 5
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 6
        height: 6
        radius: 3
        color: Theme.agentColor(chip.agent)
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: Theme.agentLabel(chip.agent)
        color: Theme.agentColor(chip.agent)
        font.pixelSize: chip.pixelSize
        font.weight: Font.Medium
    }
}

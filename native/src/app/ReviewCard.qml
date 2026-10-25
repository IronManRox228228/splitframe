import QtQuick
import QtQuick.Layouts

// A peer review from the crew: who reviewed, Approved or Needs revision, and the reason.
Rectangle {
    id: card
    property string agent: ""
    property bool approved: true
    property string reason: ""

    radius: 12
    color: Theme.cardFill
    border.width: 1
    border.color: approved ? Theme.line : Theme.dangerLine
    implicitHeight: col.implicitHeight + 20

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 10
        anchors.topMargin: 10
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Icon { name: card.approved ? "check" : "retry"; size: 14; color: card.approved ? Theme.success : Theme.warn }
            Text {
                text: card.approved ? "Review: approved" : "Review: needs revision"
                color: Theme.text
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            Item { Layout.fillWidth: true }
            AgentChip { agent: card.agent; pixelSize: 10 }
        }
        Text {
            visible: card.reason !== ""
            Layout.fillWidth: true
            text: card.reason
            color: card.approved ? Theme.muted : Theme.fg2
            font.pixelSize: 11
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }
    }
}

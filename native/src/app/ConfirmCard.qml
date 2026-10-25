import QtQuick
import QtQuick.Layouts
import "agentfmt.js" as Fmt

// Port of ConfirmCard (editor/AgentCards.tsx): Ask mode holds the edits the agent wants to make; this lists them with Apply / Skip.
// `calls` is [{name, arguments}] as agent.turn returns it in `proposed`.
Rectangle {
    id: card
    property var calls: []
    property string status: "pending" // pending | applied | skipped
    signal decided(bool apply)

    readonly property bool pending: status === "pending"
    radius: 12
    color: Theme.cardFill
    border.width: 1
    border.color: Theme.line
    implicitHeight: col.implicitHeight + 20

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 10
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 6
        Text {
            text: card.pending ? "APPLY THESE CHANGES?" : (card.status === "applied" ? "APPLIED" : "SKIPPED")
            color: Theme.muted
            font.pixelSize: 11
            font.letterSpacing: 0.6
        }
        Repeater {
            model: card.calls
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                text: Fmt.callLine(modelData)
                color: Theme.text
                font.pixelSize: 13
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            }
        }
        Text {
            visible: card.calls.length > 1
            text: card.calls.length + " changes"
            color: Theme.muted
            font.pixelSize: 11
        }
        RowLayout {
            visible: card.pending
            Layout.topMargin: 2
            spacing: 6
            GButton { text: "Apply"; variant: "accent"; small: true; onClicked: card.decided(true) }
            GButton { text: "Skip"; variant: "outline"; small: true; onClicked: card.decided(false) }
        }
    }
}

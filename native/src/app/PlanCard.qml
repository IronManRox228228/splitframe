import QtQuick
import QtQuick.Layouts
import "agentfmt.js" as Fmt

// Port of PlanCard (editor/AgentCards.tsx): the plan the crew is holding in Plan mode, step by step, with Run / Edit / Cancel while
// it is proposed. `steps` is [{intent, specialist, status}] as agent.turn returns it.
Rectangle {
    id: card
    property string summary: ""
    property var steps: []
    property string phase: "proposed" // proposed | launched | cancelled
    property bool canAct: true
    signal runRequested
    signal editRequested
    signal cancelRequested

    radius: 12
    color: Theme.cardFill
    border.width: 1
    border.color: Theme.line
    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        width: parent.width
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.bottomMargin: 8
            spacing: 8
            Icon { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; name: "layers"; size: 14; color: Theme.accent }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { Layout.fillWidth: true; text: card.summary; color: Theme.text; font.pixelSize: 13; font.weight: Font.Medium; wrapMode: Text.WrapAtWordBoundaryOrAnywhere }
                Text {
                    Layout.fillWidth: true
                    text: card.phase === "proposed" ? "Plan · " + card.steps.length + " step" + (card.steps.length === 1 ? "" : "s") + " · nothing has changed yet"
                          : card.phase === "cancelled" ? "Plan cancelled" : "Running"
                    color: Theme.muted
                    font.pixelSize: 11
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 8
            spacing: 6
            Repeater {
                model: card.steps
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 2; implicitWidth: 14; implicitHeight: 14; radius: 7; color: "transparent"; border.width: 1; border.color: Theme.lineStrong }
                    Text {
                        Layout.fillWidth: true
                        textFormat: Text.RichText
                        text: Fmt.intentLabel(modelData.intent) + " <font color='" + Theme.muted + "'>· </font><font color='" + Theme.agentColor(Fmt.specialistOf(modelData)) + "'>" + Theme.agentLabel(Fmt.specialistOf(modelData)) + "</font>"
                        color: Theme.fg2
                        font.pixelSize: 12
                    }
                }
            }
        }
        RowLayout {
            visible: card.phase === "proposed"
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.bottomMargin: 12
            Layout.topMargin: 2
            spacing: 6
            GButton { text: "Run"; iconName: "play"; variant: "accent"; small: true; enabled: card.canAct; onClicked: card.runRequested() }
            GButton { text: "Edit"; variant: "outline"; small: true; enabled: card.canAct; onClicked: card.editRequested() }
            GButton { text: "Cancel"; variant: "ghost"; small: true; onClicked: card.cancelRequested() }
            Item { Layout.fillWidth: true }
        }
        Text {
            visible: card.phase === "launched"
            Layout.leftMargin: 12
            Layout.bottomMargin: 10
            text: "Running below."
            color: Theme.muted
            font.pixelSize: 11
        }
    }
}

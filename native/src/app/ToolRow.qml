import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "agentfmt.js" as Fmt

// Port of ToolRow in editor/ChatTab.tsx: a compact tool card. A spinner while the call runs, then a check or an alert with the
// failure reason inline; the first line is the tool, under it a one-line summary; click to open the raw arguments / result. Each card
// carries the agent that made the call, in its colour.
Rectangle {
    id: row
    property string agent: ""
    property string tool: ""
    property bool pending: true
    property bool ok: true
    property string argsText: ""
    property string resultText: ""

    readonly property var parsed: Fmt.parseResult(resultText)
    readonly property string failure: pending ? "" : Fmt.failureOf(parsed, ok)
    readonly property string summary: failure !== "" ? failure.replace(/\s+/g, " ").slice(0, 140) : (pending ? "" : Fmt.summarize(parsed))
    property bool open: false

    radius: 12
    color: Theme.cardFill
    border.width: 1
    border.color: Theme.line
    clip: true
    implicitHeight: head.height + (open ? detail.height + 1 : 0)
    Behavior on implicitHeight { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }

    Rectangle {
        id: head
        width: parent.width
        height: headRow.implicitHeight + 12
        color: headArea.containsMouse ? "#0fffffff" : "transparent"
        RowLayout {
            id: headRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 6
            spacing: 8
            Item {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                implicitWidth: 14
                implicitHeight: 14
                Spinner { visible: row.pending; color: Theme.muted }
                Icon { visible: !row.pending; name: row.failure !== "" ? "alert" : "check"; size: 14; color: row.failure !== "" ? Theme.danger : Theme.success }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: Fmt.toolLabel(row.tool)
                    color: Theme.fg2
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                Text {
                    visible: row.summary !== ""
                    Layout.fillWidth: true
                    text: row.summary
                    color: row.failure !== "" ? Theme.danger : Theme.muted
                    font.pixelSize: 11
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                }
            }
            AgentChip {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                agent: row.agent
                pixelSize: 10
            }
            Icon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                name: row.open ? "chevronDown" : "chevronRight"
                size: 13
                color: Theme.faint
            }
        }
        MouseArea {
            id: headArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.open = !row.open
        }
    }

    Rectangle { visible: row.open; y: head.height; width: parent.width; height: 1; color: Theme.line }
    Item {
        id: detail
        visible: row.open
        y: head.height + 1
        width: parent.width
        height: Math.min(192, detailText.implicitHeight + 16)
        Flickable {
            anchors.fill: parent
            anchors.margins: 0
            contentWidth: width
            contentHeight: detailText.implicitHeight + 16
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Text {
                id: detailText
                x: 10
                y: 8
                width: parent.width - 20
                text: {
                    const src = row.pending || row.resultText === "" ? row.argsText : row.resultText
                    try { return JSON.stringify(JSON.parse(src), null, 1) } catch (e) { return src }
                }
                color: Theme.muted
                font.family: Theme.mono
                font.pixelSize: 11
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                textFormat: Text.PlainText
            }
        }
    }
}

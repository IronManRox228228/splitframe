import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Port of editor/AgentModePill.tsx: the agent's mode as a pill; click for the list (Plan, Ask before edits, Default, Auto). Wired to
// the engine's mode (agent.mode.set via Editor.agentMode). Shift+Tab in the chat box cycles it (the Assistant does that).
Item {
    id: pill
    property string placement: "up" // where the list opens: up | down
    property bool compact: false

    readonly property var modes: ["plan", "ask", "default", "auto"]
    readonly property var labels: ({ plan: "Plan", ask: "Ask before edits", default: "Default", auto: "Auto" })
    readonly property var hints: ({
        plan: "Reads and plans only. Nothing changes until you run the plan.",
        ask: "Shows what each change will do and waits for Apply or Skip.",
        default: "The Director plans the job and hands parts to the Editor, Colourist and Audio. Edits apply as it goes.",
        auto: "Runs the whole job without stopping. The crew reviews each other's changes."
    })
    function dot(mode) {
        return mode === "plan" ? Theme.muted : mode === "ask" ? Theme.fg2 : mode === "default" ? Theme.text : Theme.accent
    }
    function cycle() {
        appEditor.agentMode = modes[(modes.indexOf(appEditor.agentMode) + 1) % modes.length]
    }

    implicitWidth: button.implicitWidth
    implicitHeight: 28

    AbstractButton {
        id: button
        anchors.fill: parent
        focusPolicy: Qt.NoFocus
        hoverEnabled: true
        // The button sizes and places contentItem itself (anchors on it are ignored), so the side gap is padding.
        leftPadding: pill.compact ? 10 : 12
        rightPadding: pill.compact ? 10 : 12
        implicitWidth: row.implicitWidth + leftPadding + rightPadding
        onClicked: list.opened ? list.close() : list.open()
        contentItem: Row {
            id: row
            spacing: 6
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 6; height: 6; radius: 3; color: pill.dot(appEditor.agentMode) }
            Text { anchors.verticalCenter: parent.verticalCenter; text: pill.labels[appEditor.agentMode]; color: button.hovered ? Theme.text : Theme.fg2; font.pixelSize: 12 }
            Icon { anchors.verticalCenter: parent.verticalCenter; name: list.opened ? "chevronDown" : "chevronRight"; size: 11; color: Theme.faint }
        }
        background: Rectangle {
            radius: height / 2
            color: "transparent"
            border.width: 1
            border.color: button.hovered || list.opened ? Theme.text : Theme.lineStrong
        }
        ToolTip.visible: hovered && !list.opened
        ToolTip.delay: 600
        ToolTip.text: pill.hints[appEditor.agentMode] + " (Shift+Tab to change)"
    }

    Popup {
        id: list
        x: 0
        y: pill.placement === "up" ? -height - 6 : pill.height + 6
        width: 260
        padding: 6
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onOpened: Theme.modalDepth++
        onClosed: Theme.modalDepth = Math.max(0, Theme.modalDepth - 1)
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }
        background: Rectangle {
            radius: 16
            color: listGlass.active ? "transparent" : Theme.glassStrong
            border.width: 1
            border.color: Theme.glassBorder
            RefractiveGlass { id: listGlass; armed: list.visible; anchors.fill: parent; radius: parent.radius; backdrop: Theme.backdropItem; strength: 12; bevel: 14; tint: Qt.rgba(0.063, 0.07, 0.067, 0.94); blurRadius: 5 }
            Rectangle { x: parent.radius * 0.6; y: 1; width: parent.width - parent.radius * 1.2; height: 1; color: Theme.glassHighlight }
        }
        contentItem: ColumnLayout {
            spacing: 0
            Repeater {
                model: pill.modes
                delegate: Rectangle {
                    id: opt
                    required property string modelData
                    Layout.fillWidth: true
                    implicitHeight: optCol.implicitHeight + 16
                    radius: 12
                    color: optArea.containsMouse ? "#12ffffff" : (modelData === appEditor.agentMode ? "#0fffffff" : "transparent")
                    ColumnLayout {
                        id: optCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Rectangle { implicitWidth: 6; implicitHeight: 6; radius: 3; color: pill.dot(opt.modelData) }
                            Text { text: pill.labels[opt.modelData]; color: Theme.text; font.pixelSize: 13 }
                            Item { Layout.fillWidth: true }
                            Icon { visible: opt.modelData === appEditor.agentMode; name: "check"; size: 12; color: Theme.accent }
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 14
                            text: pill.hints[opt.modelData]
                            color: Theme.muted
                            font.pixelSize: 11
                            lineHeight: 1.15
                            wrapMode: Text.WordWrap
                        }
                    }
                    MouseArea {
                        id: optArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            appEditor.agentMode = opt.modelData
                            list.close()
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 6
                Layout.bottomMargin: 4
                text: "Always asks first: removing media, overwriting an export, anything that costs money, or cutting most of the timeline."
                color: Theme.faint
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }
    }
}

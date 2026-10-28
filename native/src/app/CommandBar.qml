import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Port of editor/CommandBar.tsx: the floating "Ask SplitFrame" bar over the preview. Ctrl+K focuses it; Enter hands the text to the
// Assistant (the same agent.chat path), which opens; the mode pill sits beside it. Escape drops focus.
Item {
    id: bar
    property alias field: input
    property Item backdrop: null
    signal submitted(string message)
    signal focusReleased

    implicitWidth: row.implicitWidth
    implicitHeight: 42

    function focusAndSelect() {
        input.forceActiveFocus()
        input.selectAll()
    }

    RowLayout {
        id: row
        anchors.fill: parent
        spacing: 8

        GlassPanel {
            id: inputGlass
            Layout.preferredWidth: Math.max(220, Math.min(440, bar.parent ? bar.parent.width - 32 - modeBox.width - 8 : 440))
            Layout.preferredHeight: 42
            radius: 21
            strong: true
            backdrop: bar.backdrop
            border.color: input.activeFocus ? Theme.accentRing : Theme.glassBorder
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                spacing: 10
                Icon { name: "sparkles"; size: 16; color: Theme.accent }
                TextField {
                    id: input
                    Layout.fillWidth: true
                    placeholderText: "Ask SplitFrame to edit, like “remove the ums”"
                    placeholderTextColor: Theme.faint
                    color: Theme.text
                    font.pixelSize: 13
                    background: null
                    leftPadding: 0
                    selectByMouse: true
                    // Enter sends, but never while an IME is composing
                    Keys.onReturnPressed: event => bar.submit(event)
                    Keys.onEnterPressed: event => bar.submit(event)
                    Keys.onEscapePressed: event => { bar.focusReleased(); event.accepted = true }
                }
                Rectangle {
                    implicitWidth: kbd.implicitWidth + 16
                    implicitHeight: 20
                    radius: 10
                    color: Theme.kbdFill
                    Text { id: kbd; anchors.centerIn: parent; text: "Ctrl K"; color: Theme.muted; font.family: Theme.mono; font.pixelSize: 11 }
                }
            }
        }
        GlassPanel {
            id: modeBox
            Layout.preferredWidth: pill.implicitWidth + 12
            Layout.preferredHeight: 42
            radius: 21
            strong: true
            backdrop: bar.backdrop
            AgentModePill { id: pill; anchors.centerIn: parent; placement: "down"; compact: true }
        }
    }

    // the video behind the bar moves: re-capture the small region a few times a second while the bar is on screen.
    // Not while exporting: the preview is still then, and a re-capture re-renders the window content mid-export, when the CPU is busiest
    Timer {
        interval: 250
        repeat: true
        running: bar.visible && bar.backdrop !== null && Theme.refractionOn && !appEditor.exporter.running
        onTriggered: { inputGlass.refreshBackdrop(); modeBox.refreshBackdrop() }
    }

    function submit(event) {
        if (input.inputMethodComposing) { event.accepted = false; return }
        event.accepted = true
        const message = input.text.trim()
        if (message === "") return
        input.clear()
        bar.focusReleased()
        bar.submitted(message)
    }
}

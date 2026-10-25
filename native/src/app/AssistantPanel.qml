import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "agentfmt.js" as Fmt

// Port of editor/ChatTab.tsx on the native crew. Everything shown comes from real agent.event streams and agent.chat / agent.turn
// results (Editor.agentEvent, Editor.agentTurnFinished): tool cards (spinner, then check or the failure reason, expandable), the
// agents' messages as markdown, peer reviews, plan / confirm cards for Plan and Ask mode, plain-language errors with Retry and Open
// settings, Copy, a Stop marker, IME-safe Enter. With demo mode on, the text input is replaced by clickable preset blocks.
Item {
    id: chat

    signal settingsRequested
    signal focusReleased

    property bool revising: false
    property bool userStopped: false
    property bool sawMessage: false
    property string lastUserText: ""
    property string thinkingAgent: "director"
    property var scenes: []
    property var jobs: ({})
    property var config: ({})
    readonly property bool busy: appEditor.agentBusy

    function reloadConfig() { config = appEditor.aiConfig() }
    Component.onCompleted: reloadConfig()

    readonly property string modelLine: {
        const c = config
        if (!c || !c.model) return "Loading…"
        const ep = String(c.endpoint || "")
        const where = ep.toLowerCase() === "mock" ? "mock" : ep.replace(/^https?:\/\//, "").replace(/\/.*$/, "")
        return c.model + " · " + where
    }

    // ------------------------------------------------------------------ feed
    ListModel { id: feed }
    function addRow(o) {
        feed.append(Object.assign({ kind: "", agent: "", text: "", tool: "", ok: true, pending: false, argsText: "", resultText: "", reason: "", extra: "", phase: "" }, o))
    }

    function send(text) {
        const message = String(text === undefined ? input.text : text).trim()
        if (message === "" || busy) return
        input.clear()
        revising = false
        userStopped = false
        sawMessage = false
        lastUserText = message
        addRow({ kind: "user", text: message })
        stick = true
        appEditor.agentChat(message)
    }
    function retry() {
        // drop the error card and ask again
        for (let i = feed.count - 1; i >= 0; --i) {
            if (feed.get(i).kind === "error") { feed.remove(i, feed.count - i); break }
        }
        const t = lastUserText
        if (t === "") return
        userStopped = false
        sawMessage = false
        stick = true
        appEditor.agentChat(t)
    }
    function stop() {
        appEditor.agentStop() // cancels the run: it ends after the tool call it is on and the project unlocks
        userStopped = true
        addRow({ kind: "stopped" })
    }
    function newChat() {
        userStopped = busy
        feed.clear()
        input.clear()
        revising = false
    }
    // text typed in the command bar (or a demo block): same path as typing here
    function ask(message) {
        if (busy) {
            input.text = message
            appEditor.toast("The assistant is still working. Your message is in the box, send it when it finishes.")
            return
        }
        send(message)
    }

    function findPending(tool, agent) {
        for (let i = feed.count - 1; i >= 0; --i) {
            const r = feed.get(i)
            if (r.kind === "tool" && r.pending && r.tool === tool && r.agent === agent) return i
        }
        return -1
    }

    Connections {
        target: appEditor
        function onAgentEvent(e) {
            if (chat.userStopped) return
            const agent = String(e.agent || "director")
            const kind = String(e.kind || "")
            if (kind === "think") {
                chat.thinkingAgent = agent
            } else if (kind === "tool") {
                chat.addRow({ kind: "tool", agent: agent, tool: String(e.tool || e.text || ""), pending: true, argsText: JSON.stringify(e.args || {}) })
            } else if (kind === "result") {
                const tool = String(e.tool || e.text || "")
                const i = chat.findPending(tool, agent)
                if (i >= 0) feed.set(i, { pending: false, ok: e.ok !== false, resultText: String(e.text || "") })
                else chat.addRow({ kind: "tool", agent: agent, tool: tool, pending: false, ok: e.ok !== false, resultText: String(e.text || "") })
            } else if (kind === "message") {
                chat.sawMessage = true
                chat.addRow({ kind: "message", agent: agent, text: String(e.text || "") })
            } else if (kind === "review") {
                chat.addRow({ kind: "review", agent: agent, ok: e.ok !== false, reason: String(e.text || "") })
            }
        }
        function onAgentTurnFinished(r) {
            // settle any call still spinning (the run is over)
            for (let i = 0; i < feed.count; ++i) if (feed.get(i).kind === "tool" && feed.get(i).pending) feed.set(i, { pending: false, ok: false, resultText: "" })
            if (chat.userStopped) return
            if (r.decision) {
                // Run / Apply / Skip answered: close the matching card
                for (let i = feed.count - 1; i >= 0; --i) {
                    const row = feed.get(i)
                    if ((row.kind === "plan" || row.kind === "confirm") && row.phase === "pending") {
                        feed.set(i, { phase: r.decision === "apply" ? (row.kind === "plan" ? "launched" : "applied") : (row.kind === "plan" ? "cancelled" : "skipped") })
                        break
                    }
                }
            }
            if (r.error) {
                chat.addRow({ kind: "error", text: String(r.error), reason: String(r.errorMessage || "") })
            } else if (r.planNeeded && r.plan) {
                chat.addRow({ kind: "plan", phase: "pending", text: String(r.plan.goal || ""), extra: JSON.stringify(r.plan.steps || []) })
            } else if (r.pendingApproval) {
                chat.addRow({ kind: "confirm", phase: "pending", extra: JSON.stringify(r.proposed || []) })
            } else if (!chat.sawMessage && r.response && String(r.response) !== "") {
                chat.addRow({ kind: "message", agent: "director", text: String(r.response) })
            }
            chat.sawMessage = false
        }
        function onStoryboardChanged() { sbTimer.restart() }
        function onGenerationJobChanged(j) {
            const m = Object.assign({}, chat.jobs)
            m[j.jobId] = j
            chat.jobs = m
            jobTimer.restart()
        }
    }
    Timer { id: sbTimer; interval: 200; onTriggered: if (chat.visible) chat.scenes = appEditor.storyboard() }
    onVisibleChanged: if (visible) sbTimer.restart()
    Timer {
        id: jobTimer
        interval: 3000
        onTriggered: {
            const m = {}
            for (const k in chat.jobs) if (chat.jobs[k].state === "queued" || chat.jobs[k].state === "running") m[k] = chat.jobs[k]
            chat.jobs = m
        }
    }
    readonly property var activeJobs: Object.keys(jobs).map(k => jobs[k])

    // ------------------------------------------------------------------ header
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 12
            Layout.topMargin: 16
            Layout.bottomMargin: 12
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "Assistant"; color: Theme.text; font.pixelSize: 15; font.weight: Font.DemiBold }
                Text { Layout.fillWidth: true; text: chat.modelLine; color: Theme.faint; font.pixelSize: 11; elide: Text.ElideRight }
            }
            IconBtn { small: true; iconName: "plus"; iconSize: 16; tip: "New chat"; enabled: feed.count > 0; onClicked: chat.newChat() }
            IconBtn { small: true; iconName: "settings"; iconSize: 16; tip: "AI settings"; onClicked: chat.settingsRequested() }
        }

        // ---------------------------------------------------------------- storyboard + generation
        ColumnLayout {
            visible: sb.visible || chat.activeJobs.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 10
            spacing: 8
            StoryboardStrip { id: sb; Layout.fillWidth: true; scenes: chat.scenes }
            Repeater {
                model: chat.activeJobs
                delegate: Rectangle {
                    id: job
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 40
                    radius: 12
                    color: Theme.cardFill
                    border.width: 1
                    border.color: Theme.line
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.topMargin: 7
                        anchors.bottomMargin: 7
                        spacing: 5
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Spinner { visible: job.modelData.state === "running"; size: 12 }
                            Icon { visible: job.modelData.state === "done"; name: "check"; size: 12; color: Theme.success }
                            Icon { visible: job.modelData.state === "failed"; name: "alert"; size: 12; color: Theme.danger }
                            Text { text: "Generating"; color: Theme.fg2; font.pixelSize: 12; font.weight: Font.Medium }
                            Item { Layout.fillWidth: true }
                            Text { text: Math.round((job.modelData.progress || 0) * 100) + "%"; color: Theme.muted; font.family: Theme.mono; font.pixelSize: 11 }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 4
                            radius: 2
                            color: "#26eceeec"
                            Rectangle { width: Math.max(4, parent.width * Math.min(1, job.modelData.progress || 0)); height: parent.height; radius: 2; color: Theme.accent }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------- feed
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            clip: true
            spacing: 14
            model: feed
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            rightMargin: 4
            footerPositioning: ListView.InlineFooter

            Component.onCompleted: positionViewAtEnd()
            onMovementEnded: chat.stick = atYEnd
            onContentHeightChanged: if (chat.stick) Qt.callLater(positionViewAtEnd)

            header: Item {
                width: list.width
                height: feed.count === 0 ? emptyCol.implicitHeight + 12 : 0
                visible: feed.count === 0
                ColumnLayout {
                    id: emptyCol
                    y: 12
                    width: parent.width
                    spacing: 6
                    Text { text: "Ask for an edit"; color: Theme.text; font.pixelSize: 14; font.weight: Font.Medium }
                    Text {
                        Layout.fillWidth: true
                        text: "The Director plans the job and hands parts to the Editor, Colourist and Audio. They see your footage, the selection and the playhead. Every change is a normal edit, so undo works (Ctrl+Z)."
                        color: Theme.muted
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }
            }

            footer: Item {
                width: list.width
                height: chat.busy && !chat.userStopped ? 28 : 0
                visible: chat.busy && !chat.userStopped
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 6; height: 6; radius: 3; color: Theme.agentColor(chat.thinkingAgent)
                        SequentialAnimation on opacity {
                            running: chat.busy
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.3; duration: 600 }
                            NumberAnimation { to: 1; duration: 600 }
                        }
                    }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: Theme.agentLabel(chat.thinkingAgent) + " is working…"; color: Theme.muted; font.pixelSize: 12 }
                }
            }

            delegate: Item {
                id: del
                required property int index
                required property string kind
                required property string agent
                required property string text
                required property string tool
                required property bool ok
                required property bool pending
                required property string argsText
                required property string resultText
                required property string reason
                required property string extra
                required property string phase
                width: list.width - 4
                height: loader.item ? loader.item.implicitHeight : 0
                Loader {
                    id: loader
                    width: parent.width
                    sourceComponent: {
                        switch (del.kind) {
                        case "user": return userComp
                        case "tool": return toolComp
                        case "message": return messageComp
                        case "review": return reviewComp
                        case "error": return errorComp
                        case "plan": return planComp
                        case "confirm": return confirmComp
                        case "stopped": return stoppedComp
                        }
                        return null
                    }
                }

                Component {
                    id: userComp
                    Item {
                        implicitHeight: bubble.implicitHeight
                        Rectangle {
                            id: bubble
                            anchors.right: parent.right
                            width: Math.min(parent.width * 0.85, utext.implicitWidth + 24)
                            implicitHeight: utext.implicitHeight + 20
                            height: implicitHeight
                            color: Theme.hover
                            radius: 14
                            bottomRightRadius: 4
                            Text {
                                id: utext
                                x: 12
                                y: 10
                                width: parent.width - 24
                                text: del.text
                                color: Theme.text
                                font.pixelSize: 13
                                lineHeight: 1.45
                                lineHeightMode: Text.ProportionalHeight
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            }
                        }
                    }
                }
                Component {
                    id: toolComp
                    ToolRow {
                        agent: del.agent
                        tool: del.tool
                        pending: del.pending
                        ok: del.ok
                        argsText: del.argsText
                        resultText: del.resultText
                    }
                }
                Component {
                    id: messageComp
                    ColumnLayout {
                        spacing: 6
                        AgentChip { agent: del.agent; pixelSize: 11 }
                        Markdown { Layout.fillWidth: true; source: del.text }
                        GButton {
                            small: true
                            iconName: "copy"
                            text: "Copy"
                            tip: "Copy this reply"
                            onClicked: { clip.text = del.text; clip.selectAll(); clip.copy(); appEditor.toast("Copied.") }
                        }
                    }
                }
                Component { id: reviewComp; ReviewCard { agent: del.agent; approved: del.ok; reason: del.reason } }
                Component {
                    id: errorComp
                    Rectangle {
                        id: err
                        readonly property var friendly: Fmt.friendlyError(del.text, del.reason, chat.config.endpoint)
                        property bool details: false
                        radius: 12
                        color: Theme.dangerSoft
                        border.width: 1
                        border.color: Theme.dangerLine
                        implicitHeight: ecol.implicitHeight + 24
                        ColumnLayout {
                            id: ecol
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8
                            Text { Layout.fillWidth: true; text: err.friendly.text; color: Theme.danger; font.pixelSize: 12; lineHeight: 1.2; wrapMode: Text.WordWrap }
                            RowLayout {
                                spacing: 6
                                GButton { visible: del.index === feed.count - 1; iconName: "retry"; text: "Retry"; small: true; enabled: !chat.busy; onClicked: chat.retry() }
                                GButton { iconName: "settings"; text: "Open settings"; small: true; onClicked: chat.settingsRequested() }
                            }
                            Text {
                                text: (err.details ? "▾ " : "▸ ") + "Technical details"
                                color: Theme.muted
                                font.pixelSize: 11
                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: err.details = !err.details }
                            }
                            Text {
                                visible: err.details
                                Layout.fillWidth: true
                                text: del.text + (del.reason !== "" ? "\n" + del.reason : "")
                                color: Theme.muted
                                font.family: Theme.mono
                                font.pixelSize: 11
                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            }
                        }
                    }
                }
                Component {
                    id: planComp
                    PlanCard {
                        summary: del.text
                        steps: JSON.parse(del.extra === "" ? "[]" : del.extra)
                        phase: del.phase === "pending" ? "proposed" : del.phase
                        canAct: !chat.busy
                        onRunRequested: appEditor.agentDecide(true)
                        onEditRequested: { chat.revising = true; input.forceActiveFocus() }
                        onCancelRequested: appEditor.agentDecide(false)
                    }
                }
                Component {
                    id: confirmComp
                    ConfirmCard {
                        calls: JSON.parse(del.extra === "" ? "[]" : del.extra)
                        status: del.phase
                        onDecided: apply => appEditor.agentDecide(apply)
                    }
                }
                Component {
                    id: stoppedComp
                    Row {
                        spacing: 6
                        Icon { anchors.verticalCenter: parent.verticalCenter; name: "stop"; size: 11; color: Theme.muted }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: "Stopped. The crew finishes the step it is on; nothing more is shown."; color: Theme.muted; font.pixelSize: 11 }
                    }
                }
            }
        }

        // ---------------------------------------------------------------- composer
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 12
            Layout.bottomMargin: 16
            spacing: 8

            Flow {
                Layout.fillWidth: true
                spacing: 6
                AgentModePill { id: modePill; placement: "up" }
                Repeater {
                    model: chat.busy || appEditor.demoMode ? [] : [
                        { label: "Remove silences", prompt: "Build a rough cut of my clips: remove pauses over half a second and any repeated takes." },
                        { label: "Add captions", prompt: "Add karaoke-style captions to everything I say." },
                        { label: "Cut to the beat", prompt: "Cut my video clips to the beat of the music track, one clip per beat." }
                    ]
                    delegate: Pill {
                        required property var modelData
                        text: modelData.label
                        implicitHeight: 28
                        onClicked: chat.send(modelData.prompt)
                    }
                }
            }

            // demo mode: preset blocks instead of the text box (the demo task replays a recorded run on click)
            Flow {
                visible: appEditor.demoMode
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: appEditor.demoBlocks
                    delegate: Rectangle {
                        id: block
                        required property var modelData
                        width: Math.min(blockText.implicitWidth + 28, chat.width - 32)
                        height: 40
                        radius: 14
                        color: blockArea.containsMouse ? "#14ffffff" : Theme.cardFill
                        border.width: 1
                        border.color: blockArea.containsMouse ? Theme.accentRing : Theme.lineStrong
                        Text { id: blockText; anchors.centerIn: parent; text: block.modelData.label || block.modelData.id; color: Theme.text; font.pixelSize: 13 }
                        MouseArea {
                            id: blockArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: appEditor.demoBlockClicked(block.modelData.id)
                        }
                    }
                }
            }

            Rectangle {
                visible: !appEditor.demoMode
                Layout.fillWidth: true
                implicitHeight: Math.min(140, Math.max(28, input.contentHeight + 8)) + 20
                radius: 18
                color: Theme.inputFill
                border.width: 1
                border.color: input.activeFocus ? Theme.accentRing : Theme.lineStrong

                ScrollView {
                    id: inputScroll
                    anchors.left: parent.left
                    anchors.right: sendBtn.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 12
                    anchors.rightMargin: 6
                    anchors.topMargin: 10
                    anchors.bottomMargin: 10
                    TextArea {
                        id: input
                        placeholderText: chat.revising ? "Tell me what to change in the plan…" : "Ask for an edit…"
                        placeholderTextColor: Theme.faint
                        color: Theme.text
                        font.pixelSize: 13
                        wrapMode: TextArea.Wrap
                        background: null
                        padding: 0
                        topPadding: 4
                        bottomPadding: 4
                        selectByMouse: true
                        // Enter sends, Shift+Enter breaks the line, and nothing is sent while an IME is composing
                        Keys.onReturnPressed: event => {
                            if ((event.modifiers & Qt.ShiftModifier) || input.inputMethodComposing) event.accepted = false
                            else { chat.send(); event.accepted = true }
                        }
                        Keys.onEnterPressed: event => {
                            if ((event.modifiers & Qt.ShiftModifier) || input.inputMethodComposing) event.accepted = false
                            else { chat.send(); event.accepted = true }
                        }
                        Keys.onBacktabPressed: event => { modePill.cycle(); event.accepted = true }
                        Keys.onEscapePressed: event => { chat.focusReleased(); event.accepted = true }
                    }
                }
                AbstractButton {
                    id: sendBtn
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: 10
                    anchors.bottomMargin: 10
                    width: 28
                    height: 28
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: true
                    enabled: chat.busy ? !chat.userStopped : input.text.trim() !== ""
                    onClicked: chat.busy ? chat.stop() : chat.send()
                    contentItem: Item {
                        Icon { anchors.centerIn: parent; name: chat.busy ? "stop" : "send"; size: 14; color: chat.busy ? Theme.text : Theme.ink }
                    }
                    background: Rectangle {
                        radius: 14
                        color: chat.busy ? (sendBtn.hovered ? Theme.surface600 : Theme.pressed) : (sendBtn.hovered ? Theme.accentHover : Theme.accent)
                        opacity: sendBtn.enabled ? 1 : 0.4
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: chat.busy ? "Stop" : "Send (Enter)"
                }
            }
        }
    }

    property bool stick: true
    // clipboard helper: a hidden editor that selects and copies the reply text
    TextEdit { id: clip; visible: false }
    function focusInput() { input.forceActiveFocus() }
}

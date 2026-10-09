import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SplitFrame

// Toolbar, track headers and the painted timeline, with the playhead drawn on top.
Rectangle {
    id: root
    color: Theme.bg

    property alias view: view
    readonly property int headerWidth: 176
    readonly property real logMin: Math.log(0.2)
    readonly property real logMax: Math.log(40)
    readonly property var controller: appEditor.controller

    signal focusReleased
    signal splitRequested
    signal deleteRequested
    signal cloneRequested

    function fit() { view.zoomFit() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- toolbar ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                Btn { text: "Split"; tip: "Split at playhead (S)"; onClicked: root.splitRequested() }
                Btn { text: "Delete"; enabled: appEditor.hasSelection; tip: "Delete (Del)"; onClicked: root.deleteRequested() }
                Btn { text: "Clone"; enabled: appEditor.hasSelection; tip: "Clone (Ctrl+D)"; onClicked: root.cloneRequested() }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.lineStrong }
                Btn { text: "Text"; tip: "Add a title at the playhead"; onClicked: root.controller.addText() }
                Btn { text: "Shape"; tip: "Add a shape at the playhead"; onClicked: root.controller.addShape() }
                Btn {
                    text: "+ Track"
                    tip: "Add a track"
                    onClicked: addTrackMenu.popup()
                    Menu {
                        id: addTrackMenu
                        MenuItem { text: "Video track"; onTriggered: root.controller.addTrack("video") }
                        MenuItem { text: "Audio track"; onTriggered: root.controller.addTrack("audio") }
                        MenuItem { text: "Overlay track"; onTriggered: root.controller.addTrack("overlay") }
                        MenuItem { text: "Text track"; onTriggered: root.controller.addTrack("text") }
                    }
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 18; color: Theme.lineStrong }
                Btn {
                    text: "Snap"
                    active: root.controller.snapEnabled
                    tip: "Snap to clip edges, markers and the playhead (N)"
                    onClicked: root.controller.snapEnabled = !root.controller.snapEnabled
                }
                Btn {
                    text: "Ripple"
                    active: root.controller.rippleEnabled
                    tip: "Ripple delete and trims close the gap"
                    onClicked: root.controller.rippleEnabled = !root.controller.rippleEnabled
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: appPlayer.timecode
                    color: Theme.muted
                    font.pixelSize: 11
                    font.family: Theme.mono
                }
                Btn { text: "−"; implicitHeight: 24; leftPadding: 8; rightPadding: 8; tip: "Zoom out (-)"; onClicked: view.zoomBy(1 / 1.25) }
                Slider {
                    Layout.preferredWidth: 120
                    from: 0
                    to: 1
                    focusPolicy: Qt.NoFocus
                    value: (Math.log(view.pxPerFrame) - root.logMin) / (root.logMax - root.logMin)
                    onMoved: view.pxPerFrame = Math.exp(root.logMin + value * (root.logMax - root.logMin))
                }
                Btn { text: "+"; implicitHeight: 24; leftPadding: 8; rightPadding: 8; tip: "Zoom in (+)"; onClicked: view.zoomBy(1.25) }
                Btn { text: "Fit"; tip: "Zoom to fit (Shift+Z)"; onClicked: view.zoomFit() }
            }
        }

        // ---- headers + lanes ----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // track headers
            Rectangle {
                id: headers
                width: root.headerWidth
                height: parent.height
                color: Theme.panelAlt

                Rectangle {
                    id: corner
                    width: parent.width
                    height: view.rulerHeight
                    color: Theme.panelAlt
                    Rectangle { width: parent.width; height: 1; anchors.bottom: parent.bottom; color: Theme.line }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 10
                        text: "Tracks"
                        color: Theme.faint
                        font.pixelSize: 11
                    }
                }

                ListView {
                    id: headerList
                    y: view.rulerHeight
                    width: parent.width
                    height: parent.height - view.rulerHeight
                    interactive: false
                    clip: true
                    contentY: view.scrollY
                    model: appEditor.tracks

                    delegate: Rectangle {
                        id: head
                        required property int index
                        required property string trackId
                        required property string name
                        required property string kind
                        required property bool locked
                        required property bool muted
                        required property bool hidden
                        required property int rowHeight
                        required property int itemCount
                        width: headerList.width
                        height: rowHeight
                        color: locked ? "#15171c" : Theme.panelAlt

                        Rectangle { width: parent.width; height: 1; anchors.bottom: parent.bottom; color: Theme.line }
                        Rectangle { width: 1; height: parent.height; anchors.right: parent.right; color: Theme.line }

                        Text {
                            id: kindGlyph
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            font.family: Theme.icons
                            font.pixelSize: 14
                            color: Theme.faint
                            text: head.kind === "audio" ? "" : (head.kind === "text" ? "" : (head.kind === "overlay" ? "" : ""))
                        }
                        Text {
                            id: trackName
                            x: 30
                            width: parent.width - 30 - 80
                            anchors.verticalCenter: parent.verticalCenter
                            text: head.name
                            elide: Text.ElideRight
                            color: head.locked ? Theme.faint : Theme.text
                            font.pixelSize: 12
                            visible: !renameField.visible
                        }
                        TextField {
                            id: renameField
                            visible: false
                            x: 28
                            width: parent.width - 28 - 80
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 24
                            font.pixelSize: 12
                            color: Theme.text
                            selectByMouse: true
                            background: Rectangle { radius: 4; color: Theme.field; border.color: Theme.accent }
                            onEditingFinished: {
                                if (visible) {
                                    visible = false
                                    root.controller.renameTrack(head.index, text)
                                    root.focusReleased()
                                }
                            }
                            Keys.onEscapePressed: {
                                visible = false
                                root.focusReleased()
                            }
                        }
                        Row {
                            anchors.right: parent.right
                            anchors.rightMargin: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Repeater {
                                model: [
                                    { flag: "locked", on: head.locked, glyphOn: "", glyphOff: "", tip: "Lock" },
                                    { flag: "muted", on: head.muted, glyphOn: "", glyphOff: "", tip: "Mute" },
                                    { flag: "hidden", on: head.hidden, glyphOn: "", glyphOff: "", tip: "Hide" }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    visible: modelData.flag !== "muted" || head.kind === "audio" || head.kind === "video"
                                    width: 24
                                    height: 24
                                    radius: 5
                                    color: modelData.on ? "#2c3340" : (hov.containsMouse ? Theme.hover : "transparent")
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.on ? modelData.glyphOn : modelData.glyphOff
                                        font.family: Theme.icons
                                        font.pixelSize: 13
                                        color: modelData.on ? Theme.accent : Theme.faint
                                    }
                                    MouseArea {
                                        id: hov
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            root.controller.setTrackFlag(head.index, modelData.flag, !modelData.on)
                                            root.focusReleased()
                                        }
                                    }
                                    ToolTip.visible: hov.containsMouse
                                    ToolTip.text: (modelData.on ? "Un" + modelData.tip.toLowerCase() : modelData.tip)
                                    ToolTip.delay: 600
                                }
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            z: -1
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onDoubleClicked: {
                                renameField.text = head.name
                                renameField.visible = true
                                renameField.forceActiveFocus()
                                renameField.selectAll()
                            }
                            onClicked: m => { if (m.button === Qt.RightButton) trackMenu.popup() }
                        }
                        Menu {
                            id: trackMenu
                            MenuItem {
                                text: "Rename"
                                onTriggered: {
                                    renameField.text = head.name
                                    renameField.visible = true
                                    renameField.forceActiveFocus()
                                    renameField.selectAll()
                                }
                            }
                            MenuItem { text: "Move up"; enabled: head.index > 0; onTriggered: root.controller.moveTrack(head.index, head.index - 1) }
                            MenuItem { text: "Move down"; enabled: head.index < headerList.count - 1; onTriggered: root.controller.moveTrack(head.index, head.index + 1) }
                            MenuSeparator {}
                            MenuItem {
                                text: "Remove track…"
                                onTriggered: {
                                    if (head.itemCount === 0) root.controller.removeTrack(head.index)
                                    else removeTrackDialog.ask(head.index, head.name, head.itemCount)
                                }
                            }
                        }
                    }
                }
            }

            // the lanes
            TimelineView {
                id: view
                x: root.headerWidth
                width: parent.width - root.headerWidth
                height: parent.height
                project: appEditor.project
                model: appEditor.timeline
                controller: appEditor.controller
                pool: appEditor.pool
                playhead: appPlayer.frame
                onScrubbed: frame => appEditor.seek(frame)
                onInteracted: root.focusReleased()
                onContextMenuRequested: (itemId, x, y) => clipMenu.popup(view, x, y)
                Component.onCompleted: controller.snapThresholdFrames = Math.round(8 / pxPerFrame)
            }

            // playhead: a QML overlay, so playback never repaints the clips
            Item {
                id: playhead
                readonly property real px: view.x + appPlayer.frame * view.pxPerFrame - view.scrollX
                visible: px >= view.x - 1 && px <= view.x + view.width + 1
                x: px
                width: 0
                height: parent.height
                Rectangle {
                    x: -1
                    width: 2
                    height: parent.height
                    color: Theme.accent
                }
                Rectangle {
                    x: -6
                    y: 2
                    width: 12
                    height: 12
                    radius: 6
                    color: Theme.accent
                }
            }

            ScrollBar {
                id: hbar
                x: view.x
                y: parent.height - height
                width: view.width - vbar.width
                height: 11
                orientation: Qt.Horizontal
                visible: size < 1
                policy: ScrollBar.AlwaysOn
                size: Math.min(1, view.width / Math.max(1, view.contentWidth))
                position: view.scrollX / Math.max(1, view.contentWidth)
                onPositionChanged: if (pressed) view.scrollX = position * view.contentWidth
            }
            ScrollBar {
                id: vbar
                x: parent.width - width
                y: view.rulerHeight
                height: parent.height - view.rulerHeight
                width: visible ? 11 : 0
                orientation: Qt.Vertical
                visible: size < 1
                policy: ScrollBar.AlwaysOn
                size: Math.min(1, (view.height - view.rulerHeight) / Math.max(1, view.contentHeight))
                position: view.scrollY / Math.max(1, view.contentHeight)
                onPositionChanged: if (pressed) view.scrollY = position * view.contentHeight
            }
        }
    }

    Connections {
        target: appPlayer
        function onTransportChanged() {
            if (appPlayer.playing)
                view.followPlayhead(appPlayer.frame)
        }
    }

    Menu {
        id: clipMenu
        MenuItem { text: "Split at playhead"; onTriggered: root.splitRequested() }
        MenuItem { text: "Clone"; onTriggered: root.cloneRequested() }
        MenuSeparator {}
        MenuItem { text: "Delete"; onTriggered: root.deleteRequested() }
    }

    Dialog {
        id: removeTrackDialog
        property int trackIndex: -1
        property string trackName
        property int clips: 0
        function ask(i, name, n) {
            trackIndex = i
            trackName = name
            clips = n
            open()
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Remove track?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 400
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 13
            text: "“" + removeTrackDialog.trackName + "” holds " + removeTrackDialog.clips + (removeTrackDialog.clips === 1 ? " clip" : " clips") + ". Removing the track removes them too (you can undo it)."
        }
        onAccepted: root.controller.removeTrack(trackIndex)
    }
}

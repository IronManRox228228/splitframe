import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SplitFrame

// The Edit page: the icon rail with the media pool or the Assistant, the preview with the "Ask SplitFrame" bar over it, the
// inspector and the timeline. The window drives drags and commands through the aliases and signals below.
Item {
    id: page

    property alias timeline: timeline
    property alias media: media
    property alias assistant: assistant
    property alias commandBar: commandBar
    // "media", "assistant" or "" (collapsed), as the rail shows it
    property string leftPanel: "media"

    signal importRequested
    signal focusReleased
    signal aiSettingsRequested
    signal assetDragStarted(string assetId, string name)
    signal assetDragMoved(string assetId, real globalX, real globalY)
    signal assetDropped(string assetId, real globalX, real globalY)
    signal splitRequested
    signal deleteRequested
    signal cloneRequested

    // text for the Assistant (the command bar, a script): opens the panel and sends it
    function sendToAssistant(message) {
        leftPanel = "assistant"
        assistant.ask(message)
    }
    function focusCommandBar() { commandBar.focusAndSelect() }

    SplitView {
        id: vertical
        anchors.fill: parent
        anchors.margins: Theme.gap
        orientation: Qt.Vertical
        handle: Rectangle {
            implicitWidth: Theme.gap
            implicitHeight: Theme.gap
            color: "transparent"
            Rectangle {
                anchors.centerIn: parent
                width: SplitHandle.pressed || SplitHandle.hovered ? 48 : 0
                height: 3
                radius: 2
                color: SplitHandle.pressed ? Theme.accent : Theme.lineStrong
            }
        }

        RowLayout {
            SplitView.fillHeight: true
            SplitView.minimumHeight: 240
            spacing: Theme.gap

            LeftRail {
                Layout.fillHeight: true
                Layout.preferredWidth: 76
                current: page.leftPanel
                onPicked: id => page.leftPanel = id
            }

            SplitView {
                id: upper
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal
                handle: Rectangle {
                    implicitWidth: Theme.gap
                    implicitHeight: Theme.gap
                    color: "transparent"
                    Rectangle {
                        anchors.centerIn: parent
                        width: 3
                        height: SplitHandle.pressed || SplitHandle.hovered ? 48 : 0
                        radius: 2
                        color: SplitHandle.pressed ? Theme.accent : Theme.lineStrong
                    }
                }

                // one panel open at a time; both stay built so closing the Assistant never loses the conversation
                StackLayout {
                    id: leftStack
                    visible: page.leftPanel !== ""
                    SplitView.preferredWidth: 300
                    SplitView.minimumWidth: 220
                    SplitView.maximumWidth: 520
                    currentIndex: page.leftPanel === "assistant" ? 1 : 0

                    MediaPanel {
                        id: media
                        onImportRequested: page.importRequested()
                        onFocusReleased: page.focusReleased()
                        onAssetDragStarted: (id, name) => page.assetDragStarted(id, name)
                        onAssetDragMoved: (id, gx, gy) => page.assetDragMoved(id, gx, gy)
                        onAssetDropped: (id, gx, gy) => page.assetDropped(id, gx, gy)
                    }
                    GlassPanel {
                        AssistantPanel {
                            id: assistant
                            anchors.fill: parent
                            onSettingsRequested: page.aiSettingsRequested()
                            onFocusReleased: page.focusReleased()
                        }
                    }
                }
                PreviewPanel {
                    id: preview
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 320
                }
                InspectorPanel {
                    SplitView.preferredWidth: 310
                    SplitView.minimumWidth: 240
                    SplitView.maximumWidth: 520
                    onFocusReleased: page.focusReleased()
                }
            }
        }

        TimelinePanel {
            id: timeline
            SplitView.preferredHeight: 330
            SplitView.minimumHeight: 170
            onFocusReleased: page.focusReleased()
            onSplitRequested: page.splitRequested()
            onDeleteRequested: page.deleteRequested()
            onCloneRequested: page.cloneRequested()
        }
    }

    // floats over the top of the preview, like the Electron app's
    CommandBar {
        id: commandBar
        parent: preview
        backdrop: preview.surface
        z: 10
        anchors.horizontalCenter: parent.horizontalCenter
        y: 10
        width: Math.min(preview.width - 32, 440 + 8 + 130)
        onSubmitted: message => page.sendToAssistant(message)
        onFocusReleased: page.focusReleased()
    }
}

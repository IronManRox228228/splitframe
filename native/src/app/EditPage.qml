import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SplitFrame

// The Edit page: media pool, preview, inspector and the timeline. The window drives drags and commands
// through the aliases and signals below.
Item {
    id: page

    property alias timeline: timeline
    property alias media: media

    signal importRequested
    signal focusReleased
    signal assetDragStarted(string assetId, string name)
    signal assetDragMoved(string assetId, real globalX, real globalY)
    signal assetDropped(string assetId, real globalX, real globalY)
    signal splitRequested
    signal deleteRequested
    signal cloneRequested

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

        SplitView {
            id: upper
            SplitView.fillHeight: true
            SplitView.minimumHeight: 240
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

            MediaPanel {
                id: media
                SplitView.preferredWidth: 300
                SplitView.minimumWidth: 220
                SplitView.maximumWidth: 520
                onImportRequested: page.importRequested()
                onFocusReleased: page.focusReleased()
                onAssetDragStarted: (id, name) => page.assetDragStarted(id, name)
                onAssetDragMoved: (id, gx, gy) => page.assetDragMoved(id, gx, gy)
                onAssetDropped: (id, gx, gy) => page.assetDropped(id, gx, gy)
            }
            PreviewPanel {
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
}

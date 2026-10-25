import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SplitFrame

// The preview surface with its transport bar underneath. `overlayContent` is an optional Component drawn over the
// picture's top-right corner (the Colour page's before/after).
GlassPanel {
    id: root
    property Component overlayContent
    property alias surface: surfaceRect

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            id: surfaceRect
            color: "#030404"
            radius: 14
            clip: true

            PreviewItem {
                anchors.fill: parent
                player: appPlayer
            }

            Loader {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                sourceComponent: root.overlayContent
            }

            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                text: appPlayer.stats
                color: Theme.faint
                font.pixelSize: 11
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Btn {
                text: "|<"
                tip: "Go to start (Home)"
                onClicked: appEditor.seek(0)
            }
            Btn {
                text: "<"
                tip: "Previous frame (Left)"
                onClicked: appEditor.step(-1)
            }
            Btn {
                text: appPlayer.playing ? "Pause" : "Play"
                active: appPlayer.playing
                tip: "Play / pause (Space)"
                onClicked: appPlayer.toggle()
            }
            Btn {
                text: ">"
                tip: "Next frame (Right)"
                onClicked: appEditor.step(1)
            }
            Btn {
                text: ">|"
                tip: "Go to end (End)"
                onClicked: appEditor.seek(appEditor.controller.durationFrames)
            }
            Btn {
                text: "Loop"
                active: appPlayer.loop
                onClicked: appPlayer.loop = !appPlayer.loop
            }
            Item { Layout.fillWidth: true }
            Text {
                text: appPlayer.timecode + " / " + appPlayer.durationTimecode
                color: Theme.text
                font.pixelSize: 13
                font.family: Theme.mono
            }
            Text {
                Layout.preferredWidth: 64
                horizontalAlignment: Text.AlignRight
                text: "f " + appPlayer.frame
                color: Theme.faint
                font.pixelSize: 12
                font.family: Theme.mono
            }
        }
    }
}

import QtQuick
import QtQuick.Window

Window {
    width: 1280
    height: 760
    visible: true
    color: "#0e0f12"
    title: "SplitFrame"

    Column {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Text {
            text: "SplitFrame"
            color: "#f2f2f2"
            font.pixelSize: 28
            font.weight: Font.DemiBold
        }
        Text {
            text: mediaSummary
            color: "#a0a4ad"
            font.pixelSize: 14
        }
        Rectangle {
            width: parent.width
            height: parent.height - 120
            color: "#000000"
            radius: 6

            Image {
                anchors.fill: parent
                anchors.margins: 8
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                source: mediaPath ? "image://frame/0?" + encodeURIComponent(mediaPath) : ""
            }
        }
        Text {
            text: "FFmpeg " + ffmpegVersion
            color: "#5c606a"
            font.pixelSize: 11
        }
    }
}

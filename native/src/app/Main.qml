import QtQuick
import QtQuick.Window
import SplitFrame

Window {
    id: root
    width: 1280
    height: 760
    visible: false // main() shows it once the graphics device is chosen
    color: "#0e0f12"
    title: appPlayer.loaded ? appPlayer.title + " - SplitFrame" : "SplitFrame"

    component Btn: Rectangle {
        id: btn
        property alias text: label.text
        property bool active: false
        signal clicked
        width: Math.max(34, label.implicitWidth + 18)
        height: 30
        radius: 5
        color: area.pressed ? "#3a3f4a" : (active ? "#2c3340" : (area.containsMouse ? "#262a33" : "#1b1e25"))
        border.color: active ? "#5FB7A1" : "#2c303a"
        Text {
            id: label
            anchors.centerIn: parent
            color: "#e8e8ea"
            font.pixelSize: 13
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: btn.clicked()
        }
    }

    Item {
        id: surface
        anchors.fill: parent
        focus: true

        Keys.onPressed: event => {
            const shift = event.modifiers & Qt.ShiftModifier
            switch (event.key) {
            case Qt.Key_Space: appPlayer.toggle(); break
            case Qt.Key_K: appPlayer.shuttle(0); break
            case Qt.Key_J: appPlayer.shuttle(-1); break
            case Qt.Key_L: appPlayer.shuttle(1); break
            case Qt.Key_Left: appPlayer.step(shift ? -10 : -1); break
            case Qt.Key_Right: appPlayer.step(shift ? 10 : 1); break
            case Qt.Key_Home: appPlayer.seek(0); break
            case Qt.Key_End: appPlayer.seek(appPlayer.durationFrames - 1); break
            default: return
            }
            event.accepted = true
        }

        Column {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Row {
                width: parent.width
                height: 22
                spacing: 12
                Text {
                    text: appPlayer.loaded ? appPlayer.title : "SplitFrame"
                    color: "#f2f2f2"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: appPlayer.error !== "" ? appPlayer.error : (appPlayer.loaded ? appPlayer.summary : "Pass a video, image or project .json on the command line")
                    color: appPlayer.error !== "" ? "#f08a8a" : "#a0a4ad"
                    font.pixelSize: 12
                }
            }

            Rectangle {
                width: parent.width
                height: parent.height - 22 - 46 - 20
                color: "#0a0b0d"
                radius: 6
                clip: true

                PreviewItem {
                    anchors.fill: parent
                    player: appPlayer
                }

                Text {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    visible: appPlayer.loaded
                    text: appPlayer.stats
                    color: "#8b909a"
                    font.pixelSize: 11
                }
            }

            Row {
                id: bar
                width: parent.width
                height: 36
                spacing: 8

                Btn { text: "|<"; onClicked: appPlayer.seek(0) }
                Btn { text: "<"; onClicked: appPlayer.step(-1) }
                Btn { text: appPlayer.playing ? "Pause" : "Play"; active: appPlayer.playing; onClicked: appPlayer.toggle() }
                Btn { text: ">"; onClicked: appPlayer.step(1) }
                Btn { text: ">|"; onClicked: appPlayer.seek(appPlayer.durationFrames - 1) }
                Btn { text: "Loop"; active: appPlayer.loop; onClicked: appPlayer.loop = !appPlayer.loop }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 150
                    text: appPlayer.timecode + " / " + appPlayer.durationTimecode
                    color: "#e8e8ea"
                    font.pixelSize: 13
                    font.family: "Consolas"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 70
                    text: "f " + appPlayer.frame
                    color: "#8b909a"
                    font.pixelSize: 12
                    font.family: "Consolas"
                }

                Rectangle {
                    id: track
                    anchors.verticalCenter: parent.verticalCenter
                    width: bar.width - x
                    height: 8
                    radius: 4
                    color: "#1f232b"

                    Rectangle {
                        width: appPlayer.durationFrames > 1 ? parent.width * appPlayer.frame / (appPlayer.durationFrames - 1) : 0
                        height: parent.height
                        radius: 4
                        color: "#5FB7A1"
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.topMargin: -10
                        anchors.bottomMargin: -10
                        function scrub(mouse) {
                            const t = Math.max(0, Math.min(1, mouse.x / width))
                            appPlayer.seek(Math.round(t * (appPlayer.durationFrames - 1)))
                        }
                        onPressed: mouse => scrub(mouse)
                        onPositionChanged: mouse => scrub(mouse)
                    }
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SplitFrame

// The Colour page: a large preview with before/after, the strip of the timeline's video clips, and the colour
// panel (project colour management, then the picked clip's input space and LUT). Project settings are not
// undoable (no op for them); everything on the clip is.
Item {
    id: page

    signal focusReleased

    readonly property var col: appEditor.colour
    readonly property bool has: col.clipId !== ""

    component Lbl: Text {
        color: Theme.muted
        font.pixelSize: 12
        elide: Text.ElideRight
    }
    component Section: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        color: Theme.text
        font.pixelSize: 13
        font.weight: Font.DemiBold
    }
    component Hairline: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.line
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: Theme.gap

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.gap

            PreviewPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                overlayContent: Component {
                    Rectangle {
                        implicitWidth: ba.implicitWidth + 8
                        implicitHeight: ba.implicitHeight + 8
                        radius: Theme.pill
                        color: "#b0050606"
                    Row {
                        id: ba
                        anchors.centerIn: parent
                        spacing: 4
                        Pill {
                            text: "Before"
                            active: page.col.bypass
                            tint: Theme.warn
                            onClicked: page.col.bypass = true
                        }
                        Pill {
                            text: "After"
                            active: !page.col.bypass
                            onClicked: page.col.bypass = false
                        }
                    }
                    }
                }
            }

            // ---- clip strip ----
            GlassPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: 118

                Text {
                    anchors.centerIn: parent
                    visible: strip.count === 0
                    text: "No video clips on the timeline"
                    color: Theme.faint
                    font.pixelSize: 12
                }

                ListView {
                    id: strip
                    anchors.fill: parent
                    anchors.margins: 12
                    orientation: ListView.Horizontal
                    spacing: 10
                    clip: true
                    model: page.col.clips
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded; height: 6 }

                    delegate: Item {
                        id: card
                        required property var modelData
                        width: 132
                        height: strip.height - 8

                        Rectangle {
                            anchors.fill: parent
                            radius: 12
                            color: Theme.field
                            border.width: card.modelData.selected ? 2 : 1
                            border.color: card.modelData.selected ? Theme.accent : (cardArea.containsMouse ? Theme.lineStrong : Theme.line)
                            clip: true

                            Image {
                                anchors.fill: parent
                                anchors.margins: 1
                                visible: card.modelData.thumb !== ""
                                source: card.modelData.thumb
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                                opacity: 0.85
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 24
                                color: "#cc050606"
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: tag.left
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 4
                                    text: card.modelData.name
                                    color: Theme.text
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    id: tag
                                    visible: card.modelData.hasColour
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: Theme.accent
                                }
                            }
                        }
                        MouseArea {
                            id: cardArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                page.col.selectClip(card.modelData.id)
                                page.focusReleased()
                            }
                        }
                    }
                }
            }
        }

        // ---- the colour panel ----
        GlassPanel {
            Layout.preferredWidth: 340
            Layout.fillHeight: true

            Flickable {
                anchors.fill: parent
                anchors.margins: 14
                contentWidth: width
                contentHeight: body.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; width: 6 }

                ColumnLayout {
                    id: body
                    width: parent.width
                    spacing: 8

                    Text {
                        text: "Colour"
                        color: Theme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Lbl {
                        Layout.fillWidth: true
                        visible: !page.col.available
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        text: "This build has no OpenColorIO: only the built-in sRGB / Rec.709 path exists."
                    }

                    // ---------- project ----------
                    Section { text: "Project" }
                    Lbl { text: "Working space" }
                    GlassCombo {
                        id: workingCombo
                        Layout.fillWidth: true
                        enabled: page.col.available
                        readonly property var values: page.col.workingSpaces
                        model: values.map(s => s === "" ? "Built-in linear Rec.709" : s)
                        currentIndex: Math.max(0, values.indexOf(page.col.workingSpace))
                        onActivated: i => page.col.setWorkingSpace(values[i])
                    }
                    Lbl { text: "Display / view" }
                    GlassCombo {
                        id: viewCombo
                        Layout.fillWidth: true
                        enabled: page.col.available
                        readonly property var values: [""].concat(page.col.displayViews)
                        model: values.map(s => s === "" ? "Built-in sRGB" : s)
                        currentIndex: Math.max(0, values.indexOf(page.col.displayView))
                        onActivated: i => page.col.setDisplayView(values[i])
                    }
                    Lbl { text: "Blend in" }
                    Row {
                        spacing: 6
                        Pill {
                            text: "Linear light"
                            active: page.col.blendSpace === "linear"
                            onClicked: page.col.setBlendSpace("linear")
                        }
                        Pill {
                            text: "Display"
                            active: page.col.blendSpace === "display"
                            onClicked: page.col.setBlendSpace("display")
                        }
                    }
                    Lbl {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        font.pixelSize: 11
                        color: Theme.faint
                        text: "Project colour settings are saved with the project but are not part of undo."
                    }

                    Hairline { Layout.topMargin: 6 }

                    // ---------- clip ----------
                    Section { text: page.has ? "Clip: " + page.col.clipName : "Clip" }
                    Lbl {
                        visible: !page.has
                        text: "Pick a clip in the strip."
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.has
                        spacing: 8

                        Lbl { text: "Input space" }
                        GlassCombo {
                            Layout.fillWidth: true
                            enabled: page.col.available
                            readonly property var values: [""].concat(page.col.inputSpaces)
                            model: values.map(s => s === "" ? "By stream tags" : s)
                            currentIndex: Math.max(0, values.indexOf(page.col.inputSpace))
                            onActivated: i => page.col.setInputSpace(values[i])
                        }

                        Lbl { text: "LUT" }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 30
                                radius: Theme.radiusField
                                color: Theme.field
                                border.width: 1
                                border.color: Theme.line
                                Text {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    verticalAlignment: Text.AlignVCenter
                                    text: page.col.lutName !== "" ? page.col.lutName : "None"
                                    color: page.col.lutName !== "" ? Theme.text : Theme.faint
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }
                            }
                            Btn {
                                text: "Load…"
                                enabled: page.col.available
                                tip: "Choose a .cube, .3dl, .clf or similar LUT file"
                                onClicked: lutDialog.open()
                            }
                            Btn {
                                text: "Clear"
                                enabled: page.col.lutPath !== ""
                                onClicked: page.col.clearLut()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Lbl { text: "Intensity" }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: Math.round(page.col.lutIntensity * 100) + "%"
                                color: Theme.fg2
                                font.pixelSize: 12
                                font.family: Theme.mono
                            }
                        }
                        GlassSlider {
                            Layout.fillWidth: true
                            enabled: page.col.lutPath !== ""
                            from: 0
                            to: 1
                            value: page.col.lutIntensity
                            onMoved: page.col.dragIntensity(value)
                            onReleased: page.col.endDrag()
                        }

                        Btn {
                            text: "Reset clip colour"
                            enabled: page.col.hasColour
                            onClicked: page.col.resetClip()
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: lutDialog
        title: "Choose a LUT"
        fileMode: FileDialog.OpenFile
        nameFilters: ["LUT files (*.cube *.3dl *.clf *.spi3d *.spi1d *.csp *.itx *.lut *.mga *.look)", "All files (*)"]
        onAccepted: page.col.setLut(selectedFile.toString())
    }
}

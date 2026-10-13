import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SplitFrame

// The Audio page: a compact timeline on top for context and, below it, one mixer strip per audio-capable track
// plus the master with its loudness readout. Every change goes through MixerModel (MixerSet ops: undoable).
Item {
    id: page

    signal focusReleased

    readonly property var mix: appEditor.mixer
    readonly property real noReading: -99

    function fmtDb(v) { return v <= -60 ? "-∞" : (v > 0 ? "+" : "") + v.toFixed(1) }
    function fmtLufs(v) { return v < noReading ? "—" : v.toFixed(1) }
    function fmtPan(p) {
        const n = Math.round(Math.abs(p) * 100)
        return n === 0 ? "C" : (p < 0 ? "L" : "R") + n
    }

    // ---------------------------------------------------------------- one channel strip
    component ChannelStrip: GlassPanel {
        id: strip
        property string nodeId
        property string title
        property string subtitle
        property real gainDb: 0
        property real pan: 0
        property bool muted: false
        property bool solo: false
        property bool master: false
        property bool dimmed: false
        property var inserts: []
        property real peakL: -60
        property real peakR: -60
        property real holdL: -60
        property real holdR: -60

        implicitWidth: master ? 150 : 124
        opacity: dimmed ? 0.5 : 1
        border.color: master ? Theme.lineStrong : Theme.glassBorder

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 6

            Text {
                Layout.fillWidth: true
                text: strip.title
                color: Theme.text
                font.pixelSize: 12
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: strip.subtitle
                color: Theme.faint
                font.pixelSize: 10
                font.family: Theme.mono
                elide: Text.ElideRight
            }

            // inserts: click toggles bypass
            Flow {
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                spacing: 4
                Text {
                    visible: strip.inserts.length === 0
                    text: "no inserts"
                    color: Theme.faint
                    font.pixelSize: 10
                }
                Repeater {
                    model: strip.inserts
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        width: chipLabel.implicitWidth + 14
                        height: 18
                        radius: Theme.pill
                        color: modelData.bypass ? "transparent" : Theme.accentDeep
                        border.width: 1
                        border.color: modelData.bypass ? Theme.line : Theme.accent
                        Text {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: chip.modelData.type
                            color: chip.modelData.bypass ? Theme.faint : Theme.text
                            font.pixelSize: 10
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.mix.setInsertBypass(strip.nodeId, chip.modelData.id, !chip.modelData.bypass)
                        }
                    }
                }
            }

            // pan
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                GlassSlider {
                    Layout.fillWidth: true
                    from: -1
                    to: 1
                    value: strip.pan
                    onMoved: page.mix.drag(strip.nodeId, "pan", value)
                    onReleased: page.mix.endDrag()
                    // double-click centres
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton
                        onDoubleClicked: page.mix.edit(strip.nodeId, "pan", 0)
                    }
                }
                Text {
                    Layout.preferredWidth: 28
                    horizontalAlignment: Text.AlignRight
                    text: page.fmtPan(strip.pan)
                    color: Theme.fg2
                    font.pixelSize: 10
                    font.family: Theme.mono
                }
            }

            // fader + meter
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 120
                spacing: 6
                Item { Layout.fillWidth: true }
                Fader {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 44
                    db: strip.gainDb
                    muted: strip.muted
                    onMoved: v => page.mix.drag(strip.nodeId, "gainDb", v)
                    onReleased: page.mix.endDrag()
                }
                LevelMeter {
                    Layout.fillHeight: true
                    peakL: strip.peakL
                    peakR: strip.peakR
                    holdL: strip.holdL
                    holdR: strip.holdR
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: page.fmtDb(strip.gainDb) + " dB"
                color: Theme.fg2
                font.pixelSize: 11
                font.family: Theme.mono
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 4
                Pill {
                    text: "M"
                    implicitWidth: 34
                    active: strip.muted
                    tint: Theme.danger
                    onClicked: page.mix.edit(strip.nodeId, "muted", !strip.muted)
                }
                Pill {
                    visible: !strip.master
                    text: "S"
                    implicitWidth: 34
                    active: strip.solo
                    tint: Theme.warn
                    onClicked: page.mix.edit(strip.nodeId, "solo", !strip.solo)
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: Theme.gap

        // ---------------------------------------------------------------- timeline overview
        GlassPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 176

            Item {
                anchors.fill: parent
                anchors.margins: 8

                TimelineView {
                    id: overview
                    anchors.fill: parent
                    project: appEditor.project
                    model: appEditor.timeline
                    controller: appEditor.controller
                    pool: appEditor.pool
                    playhead: appPlayer.frame
                    onScrubbed: frame => appEditor.seek(frame)
                    onInteracted: page.focusReleased()
                    onWidthChanged: Qt.callLater(zoomFit)
                    Component.onCompleted: Qt.callLater(zoomFit)
                }
                Connections {
                    target: appEditor.project
                    function onLoaded() { Qt.callLater(overview.zoomFit) }
                }
                // playhead: a QML overlay, like the main timeline's
                Rectangle {
                    readonly property real px: overview.xForFrame(appPlayer.frame)
                    visible: px >= -1 && px <= overview.width + 1
                    x: px - 1
                    width: 2
                    height: parent.height
                    color: Theme.accent
                }
            }
        }

        // ---------------------------------------------------------------- mixer
        RowLayout {
            id: mixerRow
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.gap

            GlassPanel {
                id: stripPanel
                // Sized to its strips so Master and Loudness dock right after the last one (a few tracks no
                // longer leave a wide empty band); with many tracks it fills what's left and scrolls.
                Layout.fillHeight: true
                Layout.preferredWidth: Math.max(260, stripList.contentWidth + 20)
                Layout.maximumWidth: Math.max(260, mixerRow.width - masterStrip.width - loudPanel.width - 2 * mixerRow.spacing)

                Text {
                    anchors.centerIn: parent
                    visible: stripList.count === 0
                    text: "No audio-capable tracks"
                    color: Theme.faint
                    font.pixelSize: 12
                }

                ListView {
                    id: stripList
                    anchors.fill: parent
                    anchors.margins: 10
                    orientation: ListView.Horizontal
                    spacing: 8
                    clip: true
                    model: page.mix
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded; height: 6 }

                    delegate: ChannelStrip {
                        required property string trackId
                        required property string name
                        required property string kind
                        // the model's roles fill the strip's own properties of the same name
                        required gainDb
                        required pan
                        required muted
                        required solo
                        required inserts
                        required peakL
                        required peakR
                        required holdL
                        required holdR
                        height: stripList.height
                        nodeId: trackId
                        title: name
                        subtitle: kind
                        dimmed: page.mix.anySolo && !solo
                    }
                }
            }

            // ---- master ----
            ChannelStrip {
                id: masterStrip
                Layout.fillHeight: true
                Layout.topMargin: 10 // lines up with the strips inside their panel
                Layout.bottomMargin: 10
                master: true
                nodeId: "master"
                title: "Master"
                subtitle: "output"
                gainDb: page.mix.master.gainDb
                pan: page.mix.master.pan
                muted: page.mix.master.muted
                inserts: page.mix.master.inserts
                peakL: page.mix.master.peakL
                peakR: page.mix.master.peakR
                holdL: page.mix.master.holdL
                holdR: page.mix.master.holdR
            }

            // ---- loudness ----
            GlassPanel {
                id: loudPanel
                Layout.preferredWidth: 230
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 6

                    Text {
                        text: "Loudness"
                        color: Theme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Text { text: "Momentary"; color: Theme.muted; font.pixelSize: 12; Layout.topMargin: 6 }
                    RowLayout {
                        spacing: 6
                        Text {
                            id: momentaryText
                            text: page.fmtLufs(page.mix.momentaryLufs)
                            color: page.mix.momentaryLufs > page.mix.targetLufs + 1 ? Theme.warn : Theme.text
                            font.pixelSize: 26
                            font.family: Theme.mono
                        }
                        Text { text: "LUFS"; color: Theme.faint; font.pixelSize: 11; Layout.alignment: Qt.AlignBottom; Layout.bottomMargin: 5 }
                    }
                    // -40 .. -5 LUFS, the target marked
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 8
                        radius: 4
                        color: Theme.field
                        border.width: 1
                        border.color: Theme.line
                        readonly property real lo: -40
                        readonly property real hi: -5
                        function at(v) { return Math.max(0, Math.min(1, (v - lo) / (hi - lo))) * (width - 2) }
                        Rectangle {
                            x: 1
                            y: 1
                            height: parent.height - 2
                            width: parent.at(page.mix.momentaryLufs)
                            radius: 3
                            color: page.mix.momentaryLufs > page.mix.targetLufs + 1 ? Theme.warn : Theme.accent
                        }
                        Rectangle {
                            x: parent.at(page.mix.targetLufs)
                            y: -3
                            width: 2
                            height: parent.height + 6
                            color: Theme.fg2
                        }
                    }
                    Text { text: "target " + page.mix.targetLufs + " LUFS"; color: Theme.faint; font.pixelSize: 10; font.family: Theme.mono }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; Layout.topMargin: 6; color: Theme.line }

                    Text { text: "Integrated"; color: Theme.muted; font.pixelSize: 12; Layout.topMargin: 6 }
                    RowLayout {
                        spacing: 6
                        Text {
                            text: page.mix.analysing ? "…" : page.fmtLufs(page.mix.integratedLufs)
                            color: !page.mix.analysed ? Theme.faint : (Math.abs(page.mix.integratedLufs - page.mix.targetLufs) <= 1 ? Theme.success : Theme.warn)
                            font.pixelSize: 26
                            font.family: Theme.mono
                        }
                        Text { text: "LUFS"; color: Theme.faint; font.pixelSize: 11; Layout.alignment: Qt.AlignBottom; Layout.bottomMargin: 5 }
                    }
                    Text {
                        visible: page.mix.analysed && page.mix.integratedLufs > page.noReading
                        text: {
                            const d = page.mix.integratedLufs - page.mix.targetLufs
                            return (d > 0 ? "+" : "") + d.toFixed(1) + " LU vs target  ·  true peak " + page.mix.truePeakDb.toFixed(1) + " dBTP"
                        }
                        color: Theme.muted
                        font.pixelSize: 10
                        font.family: Theme.mono
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Text {
                        visible: page.mix.analysed && page.mix.integratedLufs <= page.noReading
                        text: "Silent: nothing to measure"
                        color: Theme.muted
                        font.pixelSize: 11
                    }
                    Btn {
                        text: page.mix.analysing ? "Measuring…" : "Measure"
                        accentFill: true
                        enabled: !page.mix.analysing
                        tip: "Render the whole timeline offline and measure its integrated loudness (EBU R128)"
                        onClicked: page.mix.analyze()
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            Item { Layout.fillWidth: true } // spare width stays to the right of the mixer
        }
    }
}

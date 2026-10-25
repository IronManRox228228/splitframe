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
    // One primary-correction control: label, value, slider. Dragging is one undo step (endDrag on release);
    // double-click puts it back to neutral.
    component PrimarySlider: ColumnLayout {
        id: ps
        property string key
        property string title
        property real from: 0
        property real to: 1
        property real neutral: 0
        property int decimals: 2
        readonly property real current: page.col.primaries[key]
        Layout.fillWidth: true
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Lbl { text: ps.title }
            Item { Layout.fillWidth: true }
            Text {
                text: (ps.current > 0 && ps.neutral === 0 ? "+" : "") + ps.current.toFixed(ps.decimals)
                color: ps.current === ps.neutral ? Theme.faint : Theme.fg2
                font.pixelSize: 12
                font.family: Theme.mono
            }
        }
        GlassSlider {
            Layout.fillWidth: true
            from: ps.from
            to: ps.to
            value: ps.current
            onMoved: page.col.dragPrimary(ps.key, value)
            onReleased: page.col.endDrag()
            TapHandler { onDoubleTapped: page.col.resetPrimary(ps.key) }
        }
    }
    // Lift / gamma / gain: a master slider and, when `fine`, a slider per channel
    component GradeGroup: ColumnLayout {
        id: gg
        property string group
        property string title
        property real from: 0
        property real to: 1
        property real neutral: 0
        property bool fine: false
        Layout.fillWidth: true
        spacing: 2
        PrimarySlider { key: gg.group + ".master"; title: gg.title; from: gg.from; to: gg.to; neutral: gg.neutral }
        Repeater {
            model: gg.fine ? ["r", "g", "b"] : []
            PrimarySlider {
                required property string modelData
                key: gg.group + "." + modelData
                title: modelData.toUpperCase()
                from: gg.from
                to: gg.to
                neutral: gg.neutral
            }
        }
    }
    component Hairline: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.line
    }
    // One scope readout: small label over a mono value
    component Readout: ColumnLayout {
        property string label
        property string value
        property color tone: Theme.fg2
        Layout.fillWidth: true
        spacing: 0
        Lbl { text: parent.label; font.pixelSize: 11; Layout.fillWidth: true }
        Text {
            text: parent.value
            color: parent.tone
            font.pixelSize: 12
            font.family: Theme.mono
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    // measure only while this page is showing
    onVisibleChanged: page.col.scopesActive = page.visible
    Component.onCompleted: page.col.scopesActive = page.visible
    Component.onDestruction: page.col.scopesActive = false
    readonly property var sc: page.col.scopes
    readonly property bool measured: sc !== undefined && sc.valid === true
    function signed(v, d) { return (v > 0 ? "+" : "") + v.toFixed(d) }
    function amountTone(pct) { return pct > 5 ? Theme.danger : (pct > 1 ? Theme.warn : Theme.fg2) }
    function castText(t, tint) {
        const a = Math.abs(t) < 0.02 ? "neutral" : (t > 0 ? "warm " : "cool ") + Math.abs(t).toFixed(2)
        const b = Math.abs(tint) < 0.02 ? "" : (tint > 0 ? " / magenta " : " / green ") + Math.abs(tint).toFixed(2)
        return a + b
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
                            border.color: page.col.pickingReference && cardArea.containsMouse && !card.modelData.selected ? Theme.warn
                                          : (card.modelData.selected ? Theme.accent : (cardArea.containsMouse ? Theme.lineStrong : Theme.line))
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
                                if (page.col.pickingReference && !card.modelData.selected) {
                                    page.col.matchTo(card.modelData.id)
                                } else {
                                    page.col.pickingReference = false
                                    page.col.selectClip(card.modelData.id)
                                }
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
                    enabled: !appEditor.agentBusy // the agent holds the project (every control here is an edit)
                    opacity: appEditor.agentBusy ? 0.5 : 1

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

                    // ---------- scopes ----------
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: "Scopes"
                            color: Theme.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        Lbl {
                            text: page.col.scopesBusy ? "measuring" : (page.measured ? "frame " + page.sc.frame : "")
                            font.family: Theme.mono
                            font.pixelSize: 11
                        }
                    }
                    Rectangle {
                        id: histBox
                        Layout.fillWidth: true
                        Layout.preferredHeight: 78
                        radius: Theme.radiusField
                        color: Theme.field
                        border.width: 1
                        border.color: Theme.line
                        clip: true
                        readonly property real bw: (width - 12) / 64

                        // crushed (left) and clipped (right) zones: the bins that hold display values <= 2/255 and >= 253/255
                        Rectangle { x: 6; y: 1; width: histBox.bw; height: parent.height - 2; color: "#26F28B82" }
                        Rectangle { x: 6 + histBox.bw * 63; y: 1; width: histBox.bw; height: parent.height - 2; color: "#26F28B82" }
                        Repeater {
                            model: page.measured ? page.sc.histogram : []
                            Rectangle {
                                required property real modelData
                                required property int index
                                readonly property real norm: page.sc.histogramMax > 0 ? Math.sqrt(modelData / page.sc.histogramMax) : 0
                                x: 6 + index * histBox.bw
                                width: Math.max(1, histBox.bw - 1)
                                height: Math.max(norm > 0 ? 1 : 0, norm * (histBox.height - 10))
                                anchors.bottom: histBox.bottom
                                anchors.bottomMargin: 5
                                color: (index === 0 && page.sc.crushed > 0.05) || (index === 63 && page.sc.clipped > 0.05) ? Theme.danger : Theme.fg2
                                opacity: index === 0 || index === 63 ? 1 : 0.75
                            }
                        }
                        Lbl {
                            anchors.centerIn: parent
                            visible: !page.measured
                            text: page.col.scopesBusy ? "Measuring..." : (page.has ? "No measurement" : "Pick a clip")
                            color: Theme.faint
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        visible: page.measured
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 4
                        Readout { label: "Exposure (vs 18% grey)"; value: page.measured ? page.signed(page.sc.exposure, 2) + " stops" : "" }
                        Readout { label: "Contrast (p1 to p99)"; value: page.measured ? page.sc.contrast.toFixed(1) + " stops" : "" }
                        Readout { label: "Crushed blacks"; value: page.measured ? page.sc.crushed.toFixed(1) + " %" : ""; tone: page.measured ? page.amountTone(page.sc.crushed) : Theme.fg2 }
                        Readout { label: "Clipped highlights"; value: page.measured ? page.sc.clipped.toFixed(1) + " %" : ""; tone: page.measured ? page.amountTone(page.sc.clipped) : Theme.fg2 }
                        Readout {
                            Layout.columnSpan: 2
                            label: "Cast (warm / cool, green / magenta)"
                            value: page.measured ? page.castText(page.sc.castTemperature, page.sc.castTint) : ""
                        }
                        Readout {
                            Layout.columnSpan: 2
                            label: "Skin (share, hue off the skin line)"
                            value: page.measured ? (page.sc.skinPct < 0.05 ? "none found" : page.sc.skinPct.toFixed(1) + " %, " + page.signed(page.sc.skinOffset, 0) + " deg") : ""
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Btn {
                            text: "Auto balance"
                            enabled: page.has && !page.col.proposing
                            tip: "Measure the clip and set exposure and white balance. One undo step."
                            onClicked: page.col.autoBalance()
                        }
                        Btn {
                            text: page.col.pickingReference ? "Pick a clip..." : "Match to..."
                            active: page.col.pickingReference
                            enabled: page.has && !page.col.proposing && page.col.clips.length > 1
                            tip: "Then click the reference clip in the strip: this clip is graded to look like it."
                            onClicked: page.col.pickingReference = !page.col.pickingReference
                        }
                    }
                    Lbl {
                        Layout.fillWidth: true
                        visible: text !== ""
                        wrapMode: Text.Wrap
                        elide: Text.ElideNone
                        font.pixelSize: 11
                        color: page.col.pickingReference ? Theme.warn : Theme.faint
                        text: page.col.pickingReference ? "Click the reference clip in the strip." : page.col.status
                    }
                    Hairline { Layout.topMargin: 2 }

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

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: "Primaries"
                                color: Theme.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Item { Layout.fillWidth: true }
                            Pill {
                                id: fineToggle
                                text: "Fine RGB"
                                active: false
                                onClicked: active = !active
                            }
                            Btn {
                                text: "Reset primaries"
                                enabled: page.col.hasPrimaries
                                onClicked: page.col.resetPrimaries()
                            }
                        }
                        Lbl {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            elide: Text.ElideNone
                            font.pixelSize: 11
                            color: Theme.faint
                            text: "Linear light, after the input transform and LUT. Double-click a slider to reset it."
                        }
                        PrimarySlider { key: "exposure"; title: "Exposure (stops)"; from: -4; to: 4; neutral: 0 }
                        PrimarySlider { key: "temperature"; title: "Temperature (cool / warm)"; from: -1; to: 1; neutral: 0 }
                        PrimarySlider { key: "tint"; title: "Tint (green / magenta)"; from: -1; to: 1; neutral: 0 }
                        PrimarySlider { key: "contrast"; title: "Contrast"; from: 0; to: 2; neutral: 1 }
                        PrimarySlider { key: "saturation"; title: "Saturation"; from: 0; to: 2; neutral: 1 }
                        GradeGroup { group: "lift"; title: "Lift"; from: -0.5; to: 0.5; neutral: 0; fine: fineToggle.active }
                        GradeGroup { group: "gamma"; title: "Gamma"; from: 0.2; to: 5; neutral: 1; fine: fineToggle.active }
                        GradeGroup { group: "gain"; title: "Gain"; from: 0; to: 4; neutral: 1; fine: fineToggle.active }
                        Hairline { Layout.topMargin: 4 }
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

                        Hairline { Layout.topMargin: 4 }

                        Btn {
                            text: "Reset clip colour"
                            enabled: page.col.hasColour
                            onClicked: page.col.resetClip()
                        }
                    }

                    Hairline { Layout.topMargin: 6 }

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

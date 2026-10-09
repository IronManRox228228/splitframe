import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Properties of the selected clip(s); with nothing selected, the project's own settings. Every edit
// goes through the Inspector object (-> ops). Sliders apply live and close one undo step on release.
Rectangle {
    id: root
    color: Theme.panel

    signal focusReleased

    readonly property var insp: appEditor.inspector
    readonly property var v: insp.values
    readonly property string kind: insp.itemType
    readonly property bool visual: kind !== "audio"
    readonly property bool media: kind === "video" || kind === "audio"
    readonly property bool styled: kind === "text" || kind === "caption"
    property var pv: insp.projectValues()

    Connections {
        target: appEditor.project
        function onDocChanged() { root.pv = root.insp.projectValues() }
    }

    // ---- small building blocks ----

    component Lbl: Text {
        color: Theme.muted
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    component Section: Text {
        Layout.fillWidth: true
        Layout.topMargin: 8
        color: Theme.text
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }

    component SliderRow: RowLayout {
        id: sr
        property string label
        property string key
        property real from: 0
        property real to: 1
        property real step: 0.01
        property real value: 0
        property var fmt: function (x) { return x.toFixed(2) }
        property bool enabled: true
        Layout.fillWidth: true
        spacing: 8
        Lbl {
            text: sr.label
            Layout.preferredWidth: 64
        }
        Slider {
            Layout.fillWidth: true
            from: sr.from
            to: sr.to
            stepSize: sr.step
            value: sr.value
            enabled: sr.enabled
            focusPolicy: Qt.NoFocus
            implicitHeight: 22
            onMoved: root.insp.drag(sr.key, value)
            onPressedChanged: if (!pressed) { root.insp.endDrag(); root.focusReleased() }
        }
        Text {
            text: sr.fmt(sr.value)
            Layout.preferredWidth: 50
            horizontalAlignment: Text.AlignRight
            color: Theme.text
            font.pixelSize: 11
            font.family: Theme.mono
        }
    }

    component NumRow: RowLayout {
        id: nr
        property string label
        property string key
        property real value: 0
        property int decimals: 0
        property real min: -100000
        property real max: 100000
        property bool project: false
        Layout.fillWidth: true
        spacing: 8
        Lbl {
            text: nr.label
            Layout.preferredWidth: 64
        }
        TextField {
            id: field
            Layout.fillWidth: true
            implicitHeight: 26
            font.pixelSize: 12
            font.family: Theme.mono
            color: Theme.text
            selectByMouse: true
            horizontalAlignment: Text.AlignRight
            validator: DoubleValidator { bottom: nr.min; top: nr.max; decimals: nr.decimals; notation: DoubleValidator.StandardNotation }
            text: nr.value.toFixed(nr.decimals)
            background: Rectangle { radius: 5; color: Theme.field; border.color: field.activeFocus ? Theme.accent : Theme.line }
            onEditingFinished: {
                const n = parseFloat(text)
                if (!isNaN(n) && n !== nr.value) {
                    if (nr.project) root.insp.editProject(nr.key, n)
                    else root.insp.edit(nr.key, n)
                }
                text = Qt.binding(() => nr.value.toFixed(nr.decimals))
                root.focusReleased()
            }
        }
    }

    component ColorRow: RowLayout {
        id: cr
        property string label
        property string key
        property string value
        property bool optional: false
        property bool project: false
        Layout.fillWidth: true
        spacing: 6
        function commit(c) {
            if (cr.project) root.insp.editProject(cr.key, c)
            else root.insp.edit(cr.key, c)
        }
        Lbl {
            text: cr.label
            Layout.preferredWidth: 64
        }
        Repeater {
            model: ["#ffffff", "#ffd84d", "#ff7a59", "#0a0a0b"]
            delegate: Rectangle {
                required property string modelData
                width: 20
                height: 20
                radius: 10
                color: modelData
                border.color: cr.value.toLowerCase() === modelData ? Theme.accent : Theme.lineStrong
                border.width: cr.value.toLowerCase() === modelData ? 2 : 1
                MouseArea { anchors.fill: parent; onClicked: cr.commit(modelData) }
            }
        }
        TextField {
            id: hex
            Layout.fillWidth: true
            implicitHeight: 24
            font.pixelSize: 11
            font.family: Theme.mono
            color: Theme.text
            selectByMouse: true
            placeholderText: cr.optional ? "none" : ""
            placeholderTextColor: Theme.faint
            text: cr.value
            background: Rectangle {
                radius: 5
                color: Theme.field
                border.color: hex.activeFocus ? Theme.accent : Theme.line
                Rectangle {
                    visible: /^#[0-9a-fA-F]{6}$/.test(hex.text)
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 4
                    width: 12
                    height: 12
                    radius: 6
                    color: hex.text
                }
            }
            leftPadding: 22
            onEditingFinished: {
                if (text === "" && cr.optional) cr.commit("")
                else if (/^#[0-9a-fA-F]{6}$/.test(text) && text.toLowerCase() !== cr.value.toLowerCase()) cr.commit(text)
                text = Qt.binding(() => cr.value)
                root.focusReleased()
            }
        }
    }

    component ChoiceRow: RowLayout {
        id: ch
        property string label
        property string key
        property string value
        property var options: []   // [{ value, label }]
        Layout.fillWidth: true
        spacing: 4
        Lbl {
            text: ch.label
            Layout.preferredWidth: 64
        }
        Repeater {
            model: ch.options
            delegate: Btn {
                required property var modelData
                text: modelData.label
                implicitHeight: 24
                leftPadding: 8
                rightPadding: 8
                active: String(ch.value) === String(modelData.value)
                onClicked: root.insp.edit(ch.key, modelData.value)
            }
        }
        Item { Layout.fillWidth: true }
    }

    component CheckRow: CheckBox {
        id: cb
        property string key
        Layout.fillWidth: true
        focusPolicy: Qt.NoFocus
        implicitHeight: 24
        contentItem: Text {
            leftPadding: cb.indicator.width + 6
            text: cb.text
            color: Theme.muted
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
        }
        onClicked: root.insp.edit(key, checked)
    }

    // ---- content ----

    ScrollView {
        id: scroll
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 8

            // ======== a clip is selected ========
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.insp.count > 0
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: root.kind === "motionGraphic" ? "Motion graphic" : root.kind.charAt(0).toUpperCase() + root.kind.slice(1)
                        color: Theme.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: root.insp.count > 1 ? root.insp.count + " selected" : "1 selected"
                        color: Theme.faint
                        font.pixelSize: 11
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.v.name !== undefined ? root.v.name : ""
                    elide: Text.ElideRight
                    color: Theme.muted
                    font.pixelSize: 12
                }
                Text {
                    Layout.fillWidth: true
                    text: root.v.startTc !== undefined ? "Starts " + root.v.startTc + " · lasts " + root.v.durationTc : ""
                    color: Theme.faint
                    font.pixelSize: 11
                    font.family: Theme.mono
                }

                // ---- text ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.kind === "text"
                    spacing: 8
                    Section { text: "Text" }
                    TextArea {
                        id: textBox
                        Layout.fillWidth: true
                        Layout.preferredHeight: 70
                        color: Theme.text
                        font.pixelSize: 13
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        text: root.v.text !== undefined ? root.v.text : ""
                        background: Rectangle { radius: 5; color: Theme.field; border.color: textBox.activeFocus ? Theme.accent : Theme.line }
                        // the clip is edited when the box loses focus (the reference commits on blur too)
                        onActiveFocusChanged: {
                            if (!activeFocus) {
                                if (text.trim() !== "" && text !== root.v.text) root.insp.edit("text", text)
                                else text = Qt.binding(() => root.v.text !== undefined ? root.v.text : "")
                            }
                        }
                        Keys.onEscapePressed: {
                            text = root.v.text
                            root.focusReleased()
                        }
                    }
                }

                // ---- text / caption style ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.styled
                    spacing: 8
                    Section { text: root.kind === "caption" ? "Caption style" : "Style" }
                    NumRow { label: "Size"; key: "fontSize"; value: root.v.fontSize !== undefined ? root.v.fontSize : 0; min: 8; max: 400 }
                    ChoiceRow {
                        label: "Weight"; key: "fontWeight"; value: String(root.v.fontWeight)
                        options: [{ value: 400, label: "Regular" }, { value: 600, label: "Semi" }, { value: 700, label: "Bold" }]
                    }
                    ChoiceRow {
                        label: "Align"; key: "align"; value: root.v.align !== undefined ? root.v.align : ""
                        options: [{ value: "left", label: "Left" }, { value: "center", label: "Center" }, { value: "right", label: "Right" }]
                    }
                    ColorRow { label: "Color"; key: "color"; value: root.v.color !== undefined ? root.v.color : "" }
                    NumRow { label: "Outline"; key: "strokeWidth"; value: root.v.strokeWidth !== undefined ? root.v.strokeWidth : 0; min: 0; max: 40; decimals: 1 }
                    ColorRow { label: "Outline"; key: "strokeColor"; value: root.v.strokeColor !== undefined ? root.v.strokeColor : ""; optional: true }
                    ColorRow { label: "Box"; key: "backgroundColor"; value: root.v.backgroundColor !== undefined ? root.v.backgroundColor : ""; optional: true }
                    CheckRow { text: "Uppercase"; key: "uppercase"; checked: root.v.uppercase === true }
                }

                // ---- caption ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.kind === "caption"
                    spacing: 8
                    Section { text: "Captions" }
                    ChoiceRow {
                        label: "Mode"; key: "captionMode"; value: root.v.captionMode !== undefined ? root.v.captionMode : ""
                        options: [{ value: "word", label: "Word" }, { value: "phrase", label: "Phrase" }]
                    }
                    SliderRow {
                        label: "Words/card"; key: "maxWordsPerCard"; from: 1; to: 12; step: 1
                        value: root.v.maxWordsPerCard !== undefined ? root.v.maxWordsPerCard : 5
                        fmt: x => Math.round(x)
                    }
                    ChoiceRow {
                        label: "Highlight"; key: "highlight"; value: root.v.highlight !== undefined ? root.v.highlight : ""
                        options: [{ value: "none", label: "None" }, { value: "active-word", label: "Word" }, { value: "word-bg", label: "Box" }]
                    }
                    ColorRow { label: "Highlight"; key: "highlightColor"; value: root.v.highlightColor !== undefined ? root.v.highlightColor : "" }
                    SliderRow {
                        label: "Height"; key: "placementY"; from: 0; to: 1; step: 0.01
                        value: root.v.placementY !== undefined ? root.v.placementY : 0.8
                        fmt: x => Math.round(x * 100) + "%"
                    }
                    Text {
                        Layout.fillWidth: true
                        text: (root.v.wordCount !== undefined ? root.v.wordCount : 0) + " words from the transcript"
                        color: Theme.faint
                        font.pixelSize: 11
                    }
                }

                // ---- shape ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.kind === "shape"
                    spacing: 8
                    Section { text: "Shape" }
                    ChoiceRow {
                        label: "Shape"; key: "shape"; value: root.v.shape !== undefined ? root.v.shape : ""
                        options: [{ value: "rect", label: "Rectangle" }, { value: "ellipse", label: "Ellipse" }, { value: "triangle", label: "Triangle" }]
                    }
                    ColorRow { label: "Fill"; key: "fill"; value: root.v.fill !== undefined ? root.v.fill : "" }
                    ColorRow { label: "Stroke"; key: "stroke"; value: root.v.stroke !== undefined ? root.v.stroke : ""; optional: true }
                    NumRow { label: "Stroke w"; key: "strokeWidth"; value: root.v.strokeWidth !== undefined ? root.v.strokeWidth : 0; min: 0; max: 200; decimals: 1 }
                    NumRow { label: "Radius"; key: "radius"; value: root.v.radius !== undefined ? root.v.radius : 0; min: 0; max: 2000; decimals: 0 }
                    NumRow { label: "Width"; key: "width"; value: root.v.width !== undefined ? root.v.width : 0; min: 1; max: 20000 }
                    NumRow { label: "Height"; key: "height"; value: root.v.height !== undefined ? root.v.height : 0; min: 1; max: 20000 }
                }

                // ---- picture: transform ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.visual
                    spacing: 8
                    Section { text: "Transform" }
                    SliderRow {
                        label: "Opacity"; key: "opacity"; from: 0; to: 1; step: 0.01
                        value: root.v.opacity !== undefined ? root.v.opacity : 1
                        fmt: x => Math.round(x * 100) + "%"
                    }
                    SliderRow {
                        label: "Scale"; key: "scale"; from: 0.1; to: 4; step: 0.01
                        value: root.v.scale !== undefined ? root.v.scale : 1
                        fmt: x => Math.round(x * 100) + "%"
                    }
                    SliderRow {
                        label: "Rotation"; key: "rotation"; from: -180; to: 180; step: 1
                        value: root.v.rotation !== undefined ? root.v.rotation : 0
                        fmt: x => Math.round(x) + "°"
                    }
                    NumRow { label: "X"; key: "x"; value: root.v.x !== undefined ? root.v.x : 0; decimals: 0 }
                    NumRow { label: "Y"; key: "y"; value: root.v.y !== undefined ? root.v.y : 0; decimals: 0 }
                }

                // ---- audio-bearing: volume, speed, fades ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.media
                    spacing: 8
                    Section { text: root.kind === "audio" ? "Audio" : "Audio and timing" }
                    SliderRow {
                        label: "Volume"; key: "volume"; from: 0; to: 2; step: 0.01
                        value: root.v.volume !== undefined ? root.v.volume : 1
                        enabled: root.v.muted !== true
                        fmt: x => Math.round(x * 100) + "%"
                    }
                    CheckRow { text: "Muted"; key: "muted"; checked: root.v.muted === true }
                    SliderRow {
                        label: "Speed"; key: "speed"; from: 0.25; to: 4; step: 0.05
                        value: root.v.speed !== undefined ? root.v.speed : 1
                        fmt: x => (Math.round(x * 100) / 100) + "x"
                    }
                    SliderRow {
                        label: "Fade in"; key: "fadeIn"; from: 0; to: 120; step: 1
                        value: root.v.fadeIn !== undefined ? root.v.fadeIn : 0
                        fmt: x => Math.round(x) + " f"
                    }
                    SliderRow {
                        label: "Fade out"; key: "fadeOut"; from: 0; to: 120; step: 1
                        value: root.v.fadeOut !== undefined ? root.v.fadeOut : 0
                        fmt: x => Math.round(x) + " f"
                    }
                }

                // ---- effects ----
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.visual
                    spacing: 8
                    Section { text: "Effects" }
                    Repeater {
                        model: root.insp.effects
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 8
                            Lbl {
                                text: modelData.label
                                Layout.preferredWidth: 64
                            }
                            Slider {
                                Layout.fillWidth: true
                                from: modelData.min
                                to: modelData.max
                                value: modelData.value
                                focusPolicy: Qt.NoFocus
                                implicitHeight: 22
                                onMoved: root.insp.setEffectParam(modelData.id, value, true)
                                onPressedChanged: if (!pressed) { root.insp.endDrag(); root.focusReleased() }
                            }
                            Btn {
                                text: "✕"
                                implicitHeight: 22
                                leftPadding: 6
                                rightPadding: 6
                                tip: "Remove effect"
                                onClicked: root.insp.removeEffect(modelData.id)
                            }
                        }
                    }
                    ComboBox {
                        id: fxAdd
                        Layout.fillWidth: true
                        implicitHeight: 28
                        focusPolicy: Qt.NoFocus
                        model: root.insp.effectCatalog
                        textRole: "label"
                        displayText: "Add effect…"
                        onActivated: index => {
                            root.insp.addEffect(root.insp.effectCatalog[index].type)
                            root.focusReleased()
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: root.v.keyframes !== undefined && root.v.keyframes > 0
                    wrapMode: Text.Wrap
                    color: Theme.faint
                    font.pixelSize: 11
                    text: root.v.keyframes + (root.v.keyframes === 1 ? " property is" : " properties are") + " keyframed (shown as ◆ on the clip). Keyframes are set by the assistant; the static values above apply between them."
                }
            }

            // ======== nothing selected: the project ========
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.insp.count === 0
                spacing: 8

                Text {
                    text: "Project"
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Lbl { text: "Name"; Layout.preferredWidth: 64 }
                    TextField {
                        id: nameField
                        Layout.fillWidth: true
                        implicitHeight: 26
                        font.pixelSize: 12
                        color: Theme.text
                        selectByMouse: true
                        text: root.pv.name
                        background: Rectangle { radius: 5; color: Theme.field; border.color: nameField.activeFocus ? Theme.accent : Theme.line }
                        onEditingFinished: {
                            root.insp.editProject("name", text)
                            text = Qt.binding(() => root.pv.name)
                            root.focusReleased()
                        }
                    }
                }
                Section { text: "Canvas" }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: [
                            { label: "16:9", w: 1920, h: 1080 },
                            { label: "9:16", w: 1080, h: 1920 },
                            { label: "1:1", w: 1080, h: 1080 },
                            { label: "4:5", w: 1080, h: 1350 },
                            { label: "4K", w: 3840, h: 2160 }
                        ]
                        delegate: Btn {
                            required property var modelData
                            text: modelData.label
                            implicitHeight: 24
                            leftPadding: 8
                            rightPadding: 8
                            active: root.pv.width === modelData.w && root.pv.height === modelData.h
                            onClicked: {
                                root.insp.editProject("width", modelData.w)
                                root.insp.editProject("height", modelData.h)
                            }
                        }
                    }
                }
                NumRow { label: "Width"; key: "width"; value: root.pv.width; min: 16; max: 16384; project: true }
                NumRow { label: "Height"; key: "height"; value: root.pv.height; min: 16; max: 16384; project: true }
                Section { text: "Frame rate" }
                Row {
                    spacing: 4
                    Repeater {
                        model: [24, 25, 30, 50, 60]
                        delegate: Btn {
                            required property int modelData
                            text: modelData
                            implicitHeight: 24
                            leftPadding: 8
                            rightPadding: 8
                            active: root.pv.fps === modelData
                            onClicked: root.insp.editProject("fps", modelData)
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.faint
                    font.pixelSize: 11
                    text: "Changing the frame rate keeps every clip at the same time in seconds."
                }
                Section { text: "Background" }
                ColorRow { label: "Color"; key: "background"; value: root.pv.background; project: true }
                Text {
                    Layout.topMargin: 12
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.faint
                    font.pixelSize: 11
                    text: "Select a clip on the timeline to edit it."
                }
            }
        }
    }
}

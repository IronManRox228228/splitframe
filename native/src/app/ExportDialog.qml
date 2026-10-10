import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Export: pick a preset, a format and a file, optionally a range and loudness target, then watch it render.
// Everything runs in ExportController (its own thread); this dialog only describes the job and shows progress.
Dialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: "Export"
    width: 520
    closePolicy: appEditor.exporter.running ? Dialog.NoAutoClose : Dialog.CloseOnEscape

    readonly property var ex: appEditor.exporter
    property int duration: 0

    function options() {
        return {
            preset: presetBox.currentText,
            format: formatBox.currentText,
            quality: qualityBox.currentText,
            path: pathField.text,
            customRange: rangeCheck.checked,
            rangeIn: inSpin.value,
            rangeOut: outSpin.value,
            normalize: loudCheck.checked,
            lufs: lufsSpin.value,
            hardware: hwCheck.checked,
            overwrite: overwriteCheck.checked
        }
    }
    property string summaryText: ""
    function refresh() { summaryText = ex.summary(options()) }

    onAboutToShow: {
        const d = ex.defaults()
        duration = d.durationFrames
        if (pathField.text === "" || pathField.defaultFor !== appEditor.projectName) {
            pathField.text = d.path
            pathField.defaultFor = appEditor.projectName
        }
        inSpin.value = 0
        outSpin.value = d.durationFrames
        ex.clearResult()
        refresh()
    }

    component Label2: Text {
        color: Theme.muted
        font.pixelSize: 12
        Layout.alignment: Qt.AlignVCenter
    }
    component Combo: ComboBox {
        Layout.fillWidth: true
        implicitHeight: 28
        font.pixelSize: 12
        enabled: !root.ex.running
        onActivated: root.refresh()
    }

    contentItem: ColumnLayout {
        spacing: 10

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            Layout.fillWidth: true

            Label2 { text: "Size" }
            Combo { id: presetBox; model: root.ex.presets }

            Label2 { text: "Format" }
            Combo { id: formatBox; model: root.ex.formats }

            Label2 { text: "Quality" }
            Combo { id: qualityBox; model: ["draft", "standard", "high", "master"]; currentIndex: 1 }

            Label2 { text: "Save to" }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: pathField
                    property string defaultFor: ""
                    Layout.fillWidth: true
                    implicitHeight: 28
                    font.pixelSize: 12
                    color: Theme.text
                    selectByMouse: true
                    enabled: !root.ex.running
                    onTextEdited: root.refresh()
                    background: Rectangle { radius: 5; color: Theme.field; border.color: pathField.activeFocus ? Theme.accent : Theme.line }
                }
                Btn { text: "Browse…"; enabled: !root.ex.running; onClicked: saveFile.open() }
            }

            Label2 { text: "Range" }
            RowLayout {
                spacing: 8
                CheckBox {
                    id: rangeCheck
                    text: "Frames"
                    enabled: !root.ex.running
                    onToggled: root.refresh()
                    contentItem: Text { text: rangeCheck.text; color: Theme.text; font.pixelSize: 12; leftPadding: rangeCheck.indicator.width + 6; verticalAlignment: Text.AlignVCenter }
                }
                SpinBox { id: inSpin; from: 0; to: Math.max(0, outSpin.value - 1); editable: true; enabled: rangeCheck.checked && !root.ex.running; implicitHeight: 28; font.pixelSize: 12; onValueModified: root.refresh() }
                Label2 { text: "to" }
                SpinBox { id: outSpin; from: inSpin.value + 1; to: Math.max(1, root.duration); editable: true; enabled: rangeCheck.checked && !root.ex.running; implicitHeight: 28; font.pixelSize: 12; onValueModified: root.refresh() }
            }

            Label2 { text: "Audio" }
            RowLayout {
                spacing: 8
                CheckBox {
                    id: loudCheck
                    text: "Normalise loudness to"
                    enabled: !root.ex.running
                    contentItem: Text { text: loudCheck.text; color: Theme.text; font.pixelSize: 12; leftPadding: loudCheck.indicator.width + 6; verticalAlignment: Text.AlignVCenter }
                }
                SpinBox { id: lufsSpin; from: -40; to: -5; value: -14; editable: true; enabled: loudCheck.checked && !root.ex.running; implicitHeight: 28; font.pixelSize: 12 }
                Label2 { text: "LUFS" }
            }

            Label2 { text: "Options" }
            RowLayout {
                spacing: 14
                CheckBox {
                    id: hwCheck
                    text: "GPU encoder (NVENC)"
                    enabled: !root.ex.running
                    contentItem: Text { text: hwCheck.text; color: Theme.text; font.pixelSize: 12; leftPadding: hwCheck.indicator.width + 6; verticalAlignment: Text.AlignVCenter }
                }
                CheckBox {
                    id: overwriteCheck
                    text: "Replace existing file"
                    enabled: !root.ex.running
                    contentItem: Text { text: overwriteCheck.text; color: Theme.text; font.pixelSize: 12; leftPadding: overwriteCheck.indicator.width + 6; verticalAlignment: Text.AlignVCenter }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.summaryText
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: 11
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: root.ex.running
            from: 0
            to: 1
            value: root.ex.progress
        }
        Text {
            Layout.fillWidth: true
            visible: root.ex.running
            text: root.ex.status
            color: Theme.muted
            font.pixelSize: 11
            font.family: Theme.mono
            elide: Text.ElideRight
        }
        Text {
            Layout.fillWidth: true
            visible: !root.ex.running && root.ex.resultText !== ""
            text: root.ex.resultText
            wrapMode: Text.Wrap
            color: root.ex.resultOk ? Theme.accent : Theme.danger
            font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true
            Btn {
                visible: root.ex.resultOk && root.ex.resultPath !== ""
                text: "Show in folder"
                onClicked: root.ex.reveal(root.ex.resultPath)
            }
            Item { Layout.fillWidth: true }
            Btn {
                text: root.ex.running ? "Cancel export" : "Close"
                onClicked: root.ex.running ? root.ex.cancel() : root.close()
            }
            Btn {
                text: "Export"
                active: true
                enabled: !root.ex.running
                onClicked: root.ex.start(root.options())
            }
        }
    }

    FileDialog {
        id: saveFile
        title: "Export to"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Video and audio (*.mp4 *.mov *.mkv *.wav)", "All files (*)"]
        onAccepted: {
            pathField.text = selectedFile.toString().replace(/^file:\/\/\//, "")
            root.refresh()
        }
    }
}

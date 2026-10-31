import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Port of editor/AiSettings.tsx, rewritten for the vendor-neutral provider: the OpenAI-compatible model server (SF_LLM_ENDPOINT,
// SF_LLM_MODEL and the Fast / Deep / Judge lane models) and the ComfyUI connection. Values live in QSettings under the same keys
// the engine reads; an environment variable of the same name wins and the dialog says so. Secrets are never shown in full.
// (The Electron Models section, which downloaded transcription models, has no counterpart: nothing is downloaded here.)
GDialog {
    id: dlg
    title: "AI settings"
    dialogWidth: 520

    signal saved

    property var cfg: ({})
    property string comfyResult: ""
    property bool comfyOk: false
    property bool testing: false
    readonly property var envNames: cfg.envOverrides ? cfg.envOverrides : []

    function load() {
        cfg = appEditor.aiConfig()
        endpoint.text = cfg.endpoint || ""
        model.text = cfg.model || ""
        fast.text = cfg.modelFast || ""
        deep.text = cfg.modelDeep || ""
        judge.text = cfg.modelJudge || ""
        apiKey.text = ""
        comfyUrl.text = cfg.comfyUrl || ""
        comfyHome.text = cfg.comfyHome || ""
        comfyPython.text = cfg.comfyPython || ""
        comfyAutoStart.checked = Boolean(cfg.comfyAutoStart)
        modelsDir.text = cfg.modelsDir || ""
        routerKey.text = ""
        comfyResult = ""
        clearKey = false
        clearRouter = false
    }
    property bool clearKey: false
    property bool clearRouter: false
    onAboutToShow: load()

    function save() {
        const ok = appEditor.setAiConfig({
            endpoint: endpoint.text, model: model.text, modelFast: fast.text, modelDeep: deep.text, modelJudge: judge.text,
            apiKey: apiKey.text, clearApiKey: clearKey,
            comfyUrl: comfyUrl.text, comfyHome: comfyHome.text, comfyPython: comfyPython.text,
            comfyAutoStart: comfyAutoStart.checked,
            modelsDir: modelsDir.text,
            routerKey: routerKey.text, clearRouterKey: clearRouter
        })
        if (ok) { saved(); close() }
    }

    Connections {
        target: appEditor
        function onComfyTested(ok, message) { dlg.testing = false; dlg.comfyOk = ok; dlg.comfyResult = message }
    }

    component Lbl: Text { color: Theme.muted; font.pixelSize: 12 }
    component Note: Text { Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.WordWrap }
    component Env: Text {
        property string name
        visible: dlg.envNames.indexOf(name) >= 0
        Layout.fillWidth: true
        text: name + " is set in the environment and wins over this value."
        color: Theme.warn
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: 8
        Layout.bottomMargin: 24
        spacing: 16

        // ------------------------------------------------------------ model server
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "Model server endpoint" }
            GInput { id: endpoint; Layout.fillWidth: true; placeholderText: "http://127.0.0.1:8080/v1/chat/completions" }
            Env { name: "SF_LLM_ENDPOINT" }
            Note { text: "Any OpenAI-compatible chat server: llama-server, LM Studio, Ollama. Type mock to run the crew offline." }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "Model" }
            GInput { id: model; Layout.fillWidth: true; placeholderText: "e.g. qwythos-9b-v2" }
            Env { name: "SF_LLM_MODEL" }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: 6
                Lbl { text: "Fast model" }
                GInput { id: fast; Layout.fillWidth: true; placeholderText: "Blank = model" }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: 6
                Lbl { text: "Deep model" }
                GInput { id: deep; Layout.fillWidth: true; placeholderText: "Blank = model" }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: 6
                Lbl { text: "Judge model" }
                GInput { id: judge; Layout.fillWidth: true; placeholderText: "Blank = model" }
            }
        }
        Env { name: "SF_LLM_MODEL_FAST" }
        Env { name: "SF_LLM_MODEL_DEEP" }
        Env { name: "SF_LLM_MODEL_JUDGE" }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "API key (only if the server asks for one)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GInput {
                    id: apiKey
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: dlg.clearKey ? "Will be removed on save" : (dlg.cfg.hasApiKey ? "Saved: " + dlg.cfg.apiKeyHint : "sk-\u2026")
                }
                GButton { visible: dlg.cfg.hasApiKey === true && !dlg.clearKey; text: "Remove"; small: true; onClicked: dlg.clearKey = true }
            }
            Env { name: "SF_LLM_API_KEY" }
            Note { text: "Stored in this computer\u2019s settings and shown masked. It is only sent to the endpoint above." }
        }

        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }

        // ------------------------------------------------------------ ComfyUI
        Text { text: "ComfyUI connection"; color: Theme.text; font.pixelSize: 13; font.weight: Font.Medium }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "ComfyUI URL" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GInput { id: comfyUrl; Layout.fillWidth: true; placeholderText: "http://127.0.0.1:8188" }
                GButton {
                    text: dlg.testing ? "Testing\u2026" : "Test"
                    small: true
                    enabled: !dlg.testing
                    onClicked: { dlg.testing = true; dlg.comfyResult = ""; appEditor.testComfy(comfyUrl.text) }
                }
            }
            Text {
                visible: dlg.comfyResult !== ""
                Layout.fillWidth: true
                text: dlg.comfyResult
                color: dlg.comfyOk ? Theme.success : Theme.danger
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Env { name: "SF_COMFY_URL" }
            Note { text: "Leave blank to let SplitFrame start ComfyUI itself from the folder and Python below." }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: 6
                Lbl { text: "ComfyUI folder" }
                GInput { id: comfyHome; Layout.fillWidth: true; placeholderText: "C:\\ComfyUI" }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                spacing: 6
                Lbl { text: "Python" }
                GInput { id: comfyPython; Layout.fillWidth: true; placeholderText: "python.exe" }
            }
        }
        CheckBox {
            id: comfyAutoStart
            text: "Start ComfyUI with SplitFrame"
            contentItem: Text {
                text: comfyAutoStart.text
                color: Theme.text
                font.pixelSize: 12
                leftPadding: comfyAutoStart.indicator.width + 6
                verticalAlignment: Text.AlignVCenter
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "Models folder" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GInput { id: modelsDir; Layout.fillWidth: true; placeholderText: "C:\\dev\\sf-models" }
                GButton {
                    text: "Browse\u2026"
                    small: true
                    onClicked: modelsFolderDialog.open()
                }
            }
            Env { name: "SF_MODELS_DIR" }
            Note { text: "Directory for local ML models (hand tracking, ONNX runtime)." }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6
            Lbl { text: "Comfy Router API key (optional, for cloud models)" }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                GInput {
                    id: routerKey
                    Layout.fillWidth: true
                    echoMode: TextInput.Password
                    placeholderText: dlg.clearRouter ? "Will be removed on save" : (dlg.cfg.hasRouterKey ? "Saved: " + dlg.cfg.routerKeyHint : "Paste a key")
                }
                GButton { visible: dlg.cfg.hasRouterKey === true && !dlg.clearRouter; text: "Remove"; small: true; onClicked: dlg.clearRouter = true }
            }
            Env { name: "COMFY_API_KEY" }
        }
        Note { text: "ComfyUI settings apply the next time SplitFrame starts." }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 8
            Item { Layout.fillWidth: true }
            GButton { text: "Cancel"; variant: "ghost"; onClicked: dlg.close() }
            GButton { text: "Save"; variant: "primary"; onClicked: dlg.save() }
        }
    }

    FolderDialog {
        id: modelsFolderDialog
        title: "Select models folder"
        onAccepted: {
            let p = selectedFolder.toString()
            if (p.startsWith("file:///")) p = p.substring(8)
            else if (p.startsWith("file://")) p = p.substring(7)
            modelsDir.text = p
        }
    }
}

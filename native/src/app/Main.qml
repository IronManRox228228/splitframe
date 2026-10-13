import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import SplitFrame

ApplicationWindow {
    id: root
    width: 1440
    height: 880
    minimumWidth: 900
    minimumHeight: 600
    visible: false // main() shows it once the graphics device is chosen
    color: Theme.bg
    title: appEditor.windowTitle

    // Basic-style controls follow this palette, so menus, dialogs and tooltips come out dark
    palette {
        window: "#0D0F0E"
        windowText: "#ECEEEC"
        base: "#0D0F0E"
        alternateBase: "#0A0C0B"
        text: "#ECEEEC"
        button: "#0D0F0E"
        buttonText: "#ECEEEC"
        light: "#343A37"
        midlight: "#1B1F1D"
        mid: "#343A37"
        dark: "#1B1F1D"
        shadow: "#000000"
        highlight: "#5FB7A1"
        highlightedText: "#050606"
        placeholderText: "#78837E"
        toolTipBase: "#101211"
        toolTipText: "#ECEEEC"
    }

    // ---------------------------------------------------------------- commands
    // One Action per command: the menu, the toolbar buttons and the keyboard all run the same thing.
    component Act: Action {
        property string key
        shortcut: key
    }

    Act { id: aNew; text: "New project"; key: "Ctrl+N"; onTriggered: root.guard(() => appEditor.newProject()) }
    Act { id: aOpen; text: "Open…"; key: "Ctrl+O"; onTriggered: root.guard(() => openDialog.open()) }
    Act { id: aSave; text: "Save"; key: "Ctrl+S"; onTriggered: appEditor.save() }
    Act { id: aSaveAs; text: "Save as…"; key: "Ctrl+Shift+S"; onTriggered: saveDialog.open() }
    Act { id: aImport; text: "Import media…"; key: "Ctrl+I"; onTriggered: importDialog.open() }
    Act { id: aExport; text: "Export…"; key: "Ctrl+E"; onTriggered: exportDialog.open() }
    Act { id: aApi; text: "Local API (127.0.0.1)"; key: ""; checkable: true; checked: appEditor.apiEnabled; onTriggered: appEditor.apiEnabled = !appEditor.apiEnabled }
    Act { id: aQuit; text: "Quit"; key: "Ctrl+Q"; onTriggered: root.close() }

    Act { id: aUndo; text: "Undo"; key: "Ctrl+Z"; enabled: appEditor.canUndo; onTriggered: appEditor.undo() }
    Act { id: aRedo; text: "Redo"; key: "Ctrl+Shift+Z"; enabled: appEditor.canRedo; onTriggered: appEditor.redo() }
    Act { id: aSelectAll; text: "Select all"; key: "Ctrl+A"; onTriggered: appEditor.controller.selectAll() }
    Act { id: aDeselect; text: "Deselect"; key: "Escape"; onTriggered: appEditor.controller.deselect() }
    Act { id: aSplit; text: "Split at playhead"; key: "S"; onTriggered: appEditor.controller.splitAtPlayhead() }
    Act { id: aDelete; text: "Delete"; key: "Delete"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.deleteSelection() }
    Act { id: aClone; text: "Clone"; key: "Ctrl+D"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.cloneSelection() }
    Act { id: aNudgeL; text: "Nudge left"; key: "Alt+Left"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.nudgeSelection(-1) }
    Act { id: aNudgeR; text: "Nudge right"; key: "Alt+Right"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.nudgeSelection(1) }
    Act { id: aNudgeL10; text: "Nudge left 10 frames"; key: "Alt+Shift+Left"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.nudgeSelection(-10) }
    Act { id: aNudgeR10; text: "Nudge right 10 frames"; key: "Alt+Shift+Right"; enabled: appEditor.hasSelection; onTriggered: appEditor.controller.nudgeSelection(10) }

    Act { id: aPlay; text: "Play / pause"; key: "Space"; onTriggered: appPlayer.toggle() }
    Act { id: aShuttleBack; text: "Reverse / slower"; key: "J"; onTriggered: appPlayer.shuttle(-1) }
    Act { id: aPause; text: "Pause"; key: "K"; onTriggered: appPlayer.shuttle(0) }
    Act { id: aShuttleFwd; text: "Forward / faster"; key: "L"; onTriggered: appPlayer.shuttle(1) }
    Act { id: aStepBack; text: "Previous frame"; key: "Left"; onTriggered: appEditor.step(-1) }
    Act { id: aStepFwd; text: "Next frame"; key: "Right"; onTriggered: appEditor.step(1) }
    Act { id: aSecBack; text: "Back one second"; key: "Shift+Left"; onTriggered: appEditor.step(-Math.round(appPlayer.fps)) }
    Act { id: aSecFwd; text: "Forward one second"; key: "Shift+Right"; onTriggered: appEditor.step(Math.round(appPlayer.fps)) }
    Act { id: aHome; text: "Go to start"; key: "Home"; onTriggered: appEditor.seek(0) }
    Act { id: aEnd; text: "Go to end"; key: "End"; onTriggered: appEditor.seek(appEditor.controller.durationFrames) }
    Act { id: aPrevEdge; text: "Previous clip edge"; key: "Up"; onTriggered: appEditor.gotoEdge(-1) }
    Act { id: aNextEdge; text: "Next clip edge"; key: "Down"; onTriggered: appEditor.gotoEdge(1) }

    Act { id: aZoomIn; text: "Zoom in"; key: "Ctrl+="; onTriggered: editPage.timeline.view.zoomBy(1.25) }
    Act { id: aZoomOut; text: "Zoom out"; key: "Ctrl+-"; onTriggered: editPage.timeline.view.zoomBy(1 / 1.25) }
    Act { id: aZoomFit; text: "Zoom to fit"; key: "Shift+Z"; onTriggered: editPage.timeline.view.zoomFit() }
    Act { id: aSnap; text: "Snapping"; key: "N"; checkable: true; checked: appEditor.controller.snapEnabled; onTriggered: appEditor.controller.snapEnabled = !appEditor.controller.snapEnabled }
    Act { id: aRipple; text: "Ripple edits"; key: ""; checkable: true; checked: appEditor.controller.rippleEnabled; onTriggered: appEditor.controller.rippleEnabled = !appEditor.controller.rippleEnabled }

    // pages: Shift+5/6/7 as asked, Ctrl+1/2/3 as the safe alternative
    Shortcut { sequences: ["Ctrl+1", "Shift+5"]; onActivated: root.page = 0 }
    Shortcut { sequences: ["Ctrl+2", "Shift+6"]; onActivated: root.page = 1 }
    Shortcut { sequences: ["Ctrl+3", "Shift+7"]; onActivated: root.page = 2 }

    // second keys for commands that have two (Action holds one sequence)
    Shortcut { sequences: ["Backspace"]; onActivated: aDelete.trigger() }
    Shortcut { sequences: ["Ctrl+Y"]; onActivated: aRedo.trigger() }
    Shortcut { sequences: ["=", "+", "Ctrl++"]; onActivated: aZoomIn.trigger() }
    Shortcut { sequences: ["-"]; onActivated: aZoomOut.trigger() }

    // ---------------------------------------------------------------- pages
    // 0 Edit, 1 Colour, 2 Audio. The Colour and Audio pages are built the first time they are shown.
    readonly property var pageNames: ["edit", "colour", "audio"]
    property int page: Math.max(0, pageNames.indexOf(appOptions.page === "color" ? "colour" : appOptions.page))
    property var visited: [true, false, false]
    onPageChanged: {
        const v = visited.slice()
        v[page] = true
        visited = v
        appEditor.mixer.metering = page === 2
        keys.forceActiveFocus()
    }
    // ---------------------------------------------------------------- unsaved changes
    property var afterSave: null

    // Runs `then` now, or after the user has dealt with unsaved changes.
    function guard(then) {
        if (!appEditor.dirty) {
            then()
            return
        }
        afterSave = then
        unsavedDialog.open()
    }

    function openWithRecovery(path) {
        const rec = appEditor.recoveryFor(path)
        if (rec !== "") {
            recoverFileDialog.autosave = rec
            recoverFileDialog.path = path
            recoverFileDialog.open()
        } else {
            appEditor.openPath(path)
        }
    }

    property bool forceClose: false
    onClosing: close => {
        if (appEditor.dirty && !forceClose) {
            close.accepted = false
            guard(() => {
                forceClose = true
                root.close()
            })
        }
    }

    Connections {
        target: appEditor
        function onSaveAsRequested() { saveDialog.open() }
        function onToastRequested(text, kind) { toasts.show(text, kind) }
    }

    // ---------------------------------------------------------------- menus
    component Cmd: MenuItem {
        id: mi
        required property var act
        action: act
        contentItem: RowLayout {
            spacing: 28
            Text {
                text: mi.text
                color: mi.enabled ? Theme.text : Theme.faint
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
            Text {
                text: mi.act.key !== undefined ? mi.act.key : ""
                color: Theme.faint
                font.pixelSize: 11
            }
        }
    }

    menuBar: MenuBar {
        background: Rectangle {
            color: Theme.bg
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }
        }
        Menu {
            title: "&File"
            Cmd { act: aNew }
            Cmd { act: aOpen }
            Menu {
                id: recentMenu
                title: "Open &Recent"
                enabled: appEditor.recent.entries.length > 0
                Instantiator {
                    model: appEditor.recent.entries
                    delegate: MenuItem {
                        required property var modelData
                        text: modelData.name + (modelData.exists ? "" : "  (missing)")
                        enabled: modelData.exists
                        onTriggered: root.guard(() => root.openWithRecovery(modelData.path))
                    }
                    onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => recentMenu.removeItem(object)
                }
            }
            MenuSeparator {}
            Cmd { act: aSave }
            Cmd { act: aSaveAs }
            MenuSeparator {}
            Cmd { act: aImport }
            Cmd { act: aExport }
            MenuSeparator {}
            Cmd { act: aApi }
            MenuSeparator {}
            Cmd { act: aQuit }
        }
        Menu {
            title: "&Edit"
            Cmd { act: aUndo }
            Cmd { act: aRedo }
            MenuSeparator {}
            Cmd { act: aSelectAll }
            Cmd { act: aDeselect }
            MenuSeparator {}
            Cmd { act: aSplit }
            Cmd { act: aClone }
            Cmd { act: aDelete }
            Cmd { act: aNudgeL }
            Cmd { act: aNudgeR }
        }
        Menu {
            title: "&Playback"
            Cmd { act: aPlay }
            Cmd { act: aShuttleBack }
            Cmd { act: aPause }
            Cmd { act: aShuttleFwd }
            MenuSeparator {}
            Cmd { act: aStepBack }
            Cmd { act: aStepFwd }
            Cmd { act: aSecBack }
            Cmd { act: aSecFwd }
            Cmd { act: aHome }
            Cmd { act: aEnd }
            Cmd { act: aPrevEdge }
            Cmd { act: aNextEdge }
        }
        Menu {
            title: "&Timeline"
            Cmd { act: aZoomIn }
            Cmd { act: aZoomOut }
            Cmd { act: aZoomFit }
            MenuSeparator {}
            Cmd { act: aSnap }
            Cmd { act: aRipple }
        }
    }

    // ---------------------------------------------------------------- top bar
    header: Item {
        height: 46
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

        RowLayout {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 14
            spacing: 8
            Rectangle {
                width: 8
                height: 8
                radius: 4
                color: appEditor.dirty ? Theme.warn : Theme.accent
            }
            Text {
                text: appEditor.projectName
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
        }

        PageSwitcher {
            anchors.centerIn: parent
            current: root.page
            onPicked: i => root.page = i
        }

        RowLayout {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 14
            spacing: 6
            Btn { text: "Undo"; enabled: appEditor.canUndo; tip: "Undo (Ctrl+Z)"; onClicked: aUndo.trigger() }
            Btn { text: "Redo"; enabled: appEditor.canRedo; tip: "Redo (Ctrl+Shift+Z)"; onClicked: aRedo.trigger() }
            Btn { text: "Export"; accentFill: true; tip: "Export (Ctrl+E)"; onClicked: aExport.trigger() }
        }
    }

    // ---------------------------------------------------------------- layout
    // keyboard focus lives here unless a text field has it
    Item {
        id: keys
        anchors.fill: parent
        focus: true

        StackLayout {
            anchors.fill: parent
            currentIndex: root.page

            EditPage {
                id: editPage
                onImportRequested: importDialog.open()
                onFocusReleased: keys.forceActiveFocus()
                onAssetDragStarted: (id, name) => {
                    root.draggedAsset = id
                    ghostLabel.text = name
                }
                onAssetDragMoved: (id, gx, gy) => root.poolDrag(id, gx, gy)
                onAssetDropped: (id, gx, gy) => root.poolDrop(id, gx, gy)
                onSplitRequested: aSplit.trigger()
                onDeleteRequested: aDelete.trigger()
                onCloneRequested: aClone.trigger()
            }
            Loader {
                active: root.visited[1]
                sourceComponent: ColourPage { onFocusReleased: keys.forceActiveFocus() }
            }
            Loader {
                active: root.visited[2]
                sourceComponent: AudioPage { onFocusReleased: keys.forceActiveFocus() }
            }
        }
    }

    // ---------------------------------------------------------------- dragging media from the pool
    property string draggedAsset: ""

    function inTimeline(p) {
        return p.x >= 0 && p.y >= 0 && p.x < editPage.timeline.view.width && p.y < editPage.timeline.view.height
    }
    function poolDrag(id, gx, gy) {
        const p = keys.mapFromGlobal(gx, gy)
        ghost.x = p.x + 10
        ghost.y = p.y + 10
        const lp = editPage.timeline.view.mapFromGlobal(gx, gy)
        if (inTimeline(lp)) editPage.timeline.view.showDropPreview(lp.x, lp.y, id)
        else editPage.timeline.view.clearDropPreview()
    }
    function poolDrop(id, gx, gy) {
        const lp = editPage.timeline.view.mapFromGlobal(gx, gy)
        editPage.timeline.view.clearDropPreview()
        draggedAsset = ""
        if (inTimeline(lp)) editPage.timeline.view.dropAsset(lp.x, lp.y, id)
        keys.forceActiveFocus()
    }

    Rectangle {
        id: ghost
        parent: keys
        z: 1000
        visible: root.draggedAsset !== ""
        width: ghostLabel.implicitWidth + 20
        height: 26
        radius: 6
        color: "#e61b1e25"
        border.color: Theme.accent
        Text {
            id: ghostLabel
            anchors.centerIn: parent
            color: Theme.text
            font.pixelSize: 12
        }
    }

    // files dragged in from Explorer
    DropArea {
        anchors.fill: parent
        z: -1
        onDropped: drop => {
            if (!drop.hasUrls) return
            const urls = []
            for (let i = 0; i < drop.urls.length; ++i) urls.push(drop.urls[i].toString())
            if (urls.length === 1 && urls[0].toLowerCase().endsWith(".json")) root.guard(() => root.openWithRecovery(urls[0]))
            else appEditor.importFiles(urls)
        }
    }

    Toasts {
        id: toasts
        parent: keys
        z: 900
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        onUndoClicked: appEditor.undo()
    }

    // ---------------------------------------------------------------- dialogs
    FileDialog {
        id: openDialog
        title: "Open project or media"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Projects and media (*.json *.mp4 *.mov *.mkv *.webm *.avi *.m4v *.mp3 *.wav *.m4a *.flac *.png *.jpg *.jpeg)", "SplitFrame projects (*.json)", "All files (*)"]
        onAccepted: root.openWithRecovery(selectedFile.toString())
    }
    FileDialog {
        id: saveDialog
        title: "Save project"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["SplitFrame projects (*.json)"]
        onAccepted: {
            if (appEditor.saveAs(selectedFile.toString()) && root.afterSave) {
                const f = root.afterSave
                root.afterSave = null
                f()
            }
        }
        onRejected: root.afterSave = null
    }
    FileDialog {
        id: importDialog
        title: "Import media"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Media (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.mp3 *.wav *.m4a *.flac *.aac *.ogg *.png *.jpg *.jpeg *.webp *.bmp)", "All files (*)"]
        onAccepted: {
            const urls = []
            for (let i = 0; i < selectedFiles.length; ++i) urls.push(selectedFiles[i].toString())
            appEditor.importFiles(urls)
        }
    }

    ExportDialog { id: exportDialog }

    Dialog {
        id: unsavedDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Unsaved changes"
        width: 440
        standardButtons: Dialog.Save | Dialog.Discard | Dialog.Cancel
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 13
            text: "“" + appEditor.projectName + "” has changes that are not saved."
        }
        onAccepted: {
            // Save: with a file name it saves and goes on; without one the save-as dialog continues the job
            if (appEditor.projectPath !== "") {
                if (appEditor.save()) {
                    const f = root.afterSave
                    root.afterSave = null
                    if (f) f()
                }
            } else {
                saveDialog.open()
            }
        }
        onDiscarded: {
            close()
            const f = root.afterSave
            root.afterSave = null
            if (f) f()
        }
        onRejected: root.afterSave = null
    }

    Dialog {
        id: recoverFileDialog
        property string autosave
        property string path
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Recover unsaved changes?"
        width: 460
        standardButtons: Dialog.Yes | Dialog.Discard | Dialog.Cancel
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 13
            text: "SplitFrame found an autosave of this project that is newer than the file. Yes opens the autosaved version; Discard opens the file as last saved and deletes the autosave."
        }
        onAccepted: appEditor.restoreRecovery(autosave)
        onDiscarded: {
            appEditor.discardRecovery(autosave)
            appEditor.openPath(path)
        }
    }

    Dialog {
        id: startupRecoveryDialog
        property var entry: ({})
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Restore unsaved project?"
        width: 460
        standardButtons: Dialog.Yes | Dialog.Discard | Dialog.Cancel
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 13
            text: "SplitFrame closed before “" + (startupRecoveryDialog.entry.name || "") + "” was saved (autosaved " + (startupRecoveryDialog.entry.modified || "") + "). Restore it?"
        }
        onAccepted: appEditor.restoreRecovery(entry.autosavePath)
        onDiscarded: appEditor.discardRecovery(entry.autosavePath)
    }

    Component.onCompleted: {
        const v = visited.slice()
        v[page] = true
        visited = v
        appEditor.mixer.metering = page === 2
        keys.forceActiveFocus()
        if (appOptions.startup) {
            const found = appEditor.recoveries()
            if (found.length > 0) {
                startupRecoveryDialog.entry = found[0]
                startupRecoveryDialog.open()
            }
        }
    }

    // a freshly opened project is shown whole when it would not fit at the current zoom
    Connections {
        target: appEditor.project
        function onLoaded() {
            Qt.callLater(() => {
                const v = editPage.timeline.view
                if (appOptions.fit || appEditor.controller.durationFrames * v.pxPerFrame > v.width - 40)
                    v.zoomFit()
                else
                    v.scrollX = 0
            })
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The media pool: import button, search and kind filter, and a grid of cards. Cards drag onto the
// timeline (the window maps the pointer; this panel only reports where it is) and double-click adds
// the clip at the playhead.
Rectangle {
    id: root
    color: Theme.panel

    signal importRequested
    signal assetDragStarted(string assetId, string name)
    signal assetDragMoved(string assetId, real globalX, real globalY)
    signal assetDropped(string assetId, real globalX, real globalY)
    signal focusReleased

    readonly property var pool: appEditor.pool

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Media"
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                visible: root.pool.pending > 0
                text: "analysing…"
                color: Theme.faint
                font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Btn {
                text: "Import"
                tip: "Import media (Ctrl+I)"
                onClicked: root.importRequested()
            }
        }

        TextField {
            id: search
            Layout.fillWidth: true
            placeholderText: "Search by file name"
            color: Theme.text
            placeholderTextColor: Theme.faint
            font.pixelSize: 12
            selectByMouse: true
            leftPadding: 10
            implicitHeight: 30
            background: Rectangle {
                radius: 15
                color: Theme.field
                border.color: search.activeFocus ? Theme.accent : Theme.line
            }
            onTextChanged: root.pool.filterText = text
            onEditingFinished: root.focusReleased()
        }

        Row {
            spacing: 6
            Repeater {
                model: [{ id: "all", label: "All" }, { id: "video", label: "Video" }, { id: "audio", label: "Audio" }, { id: "image", label: "Images" }]
                delegate: Btn {
                    required property var modelData
                    text: modelData.label
                    implicitHeight: 24
                    active: (root.pool.filterKind === "" ? "all" : root.pool.filterKind) === modelData.id
                    onClicked: root.pool.filterKind = modelData.id
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: grid
                anchors.fill: parent
                clip: true
                model: root.pool
                cellWidth: Math.floor(width / 2)
                cellHeight: 118
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: card
                    required property int index
                    required property string assetId
                    required property string name
                    required property string kind
                    required property string status
                    required property string duration
                    required property string detail
                    required property string thumb
                    required property bool missing
                    required property int uses
                    width: grid.cellWidth
                    height: grid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 8
                        color: Theme.raised
                        border.color: ma.containsMouse ? Theme.lineStrong : Theme.line

                        Rectangle {
                            id: poster
                            x: 4
                            y: 4
                            width: parent.width - 8
                            height: 66
                            radius: 5
                            color: card.kind === "audio" ? "#1d3b37" : "#10201c"
                            clip: true

                            Image {
                                anchors.fill: parent
                                visible: card.thumb !== ""
                                source: card.thumb
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: card.thumb === ""
                                text: card.kind === "audio" ? "" : (card.kind === "image" ? "" : "")
                                font.family: Theme.icons
                                font.pixelSize: 24
                                color: Theme.faint
                            }
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 4
                                width: dur.implicitWidth + 8
                                height: 15
                                radius: 7
                                color: "#cc08090b"
                                Text {
                                    id: dur
                                    anchors.centerIn: parent
                                    text: card.duration
                                    color: Theme.text
                                    font.pixelSize: 10
                                    font.family: Theme.mono
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                visible: card.status === "importing" || card.status === "processing" || card.status === "failed" || card.missing
                                color: "#99000000"
                                Text {
                                    anchors.centerIn: parent
                                    width: parent.width - 8
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    font.pixelSize: 11
                                    color: card.status === "failed" || card.missing ? Theme.danger : Theme.muted
                                    text: card.missing ? "File not found" : (card.status === "failed" ? "Can't read" : "Reading…")
                                }
                            }
                        }
                        Text {
                            x: 8
                            y: 74
                            width: parent.width - 16
                            text: card.name
                            elide: Text.ElideMiddle
                            color: Theme.text
                            font.pixelSize: 12
                        }
                        Text {
                            x: 8
                            y: 91
                            width: parent.width - 16
                            text: card.uses > 0 ? card.detail + " · " + card.uses + (card.uses === 1 ? " clip" : " clips") : card.detail
                            elide: Text.ElideRight
                            color: Theme.faint
                            font.pixelSize: 10
                        }
                    }

                    MouseArea {
                        id: ma
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property bool dragging: false
                        property point origin
                        onPressed: m => {
                            origin = Qt.point(m.x, m.y)
                            dragging = false
                            root.focusReleased()
                        }
                        onPositionChanged: m => {
                            if (!pressed || !(pressedButtons & Qt.LeftButton))
                                return
                            if (!dragging && Math.hypot(m.x - origin.x, m.y - origin.y) > 8) {
                                dragging = true
                                root.assetDragStarted(card.assetId, card.name)
                            }
                            if (dragging) {
                                const g = ma.mapToGlobal(m.x, m.y)
                                root.assetDragMoved(card.assetId, g.x, g.y)
                            }
                        }
                        onReleased: m => {
                            if (dragging) {
                                const g = ma.mapToGlobal(m.x, m.y)
                                root.assetDropped(card.assetId, g.x, g.y)
                            }
                            dragging = false
                        }
                        onClicked: m => {
                            if (m.button === Qt.RightButton)
                                cardMenu.popup()
                        }
                        onDoubleClicked: appEditor.controller.addAsset(card.assetId, appPlayer.frame, -1)
                    }

                    Menu {
                        id: cardMenu
                        MenuItem {
                            text: "Add to timeline at playhead"
                            onTriggered: appEditor.controller.addAsset(card.assetId, appPlayer.frame, -1)
                        }
                        MenuItem {
                            text: "Refresh"
                            onTriggered: root.pool.refreshAvailability()
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Remove from project…"
                            onTriggered: {
                                if (card.uses === 0)
                                    root.pool.removeAsset(card.assetId, false)
                                else
                                    removeDialog.ask(card.assetId, card.name, card.uses)
                            }
                        }
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 24
                spacing: 8
                visible: grid.count === 0
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.text
                    font.pixelSize: 13
                    text: root.pool.filterText !== "" || root.pool.filterKind !== "" && root.pool.filterKind !== "all" ? "No matches" : "No media yet"
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    color: Theme.faint
                    font.pixelSize: 11
                    text: "Import video, audio or images, or drop files here."
                }
            }
        }
    }

    Dialog {
        id: removeDialog
        property string assetId
        property string assetName
        property int uses: 0
        function ask(id, name, n) {
            assetId = id
            assetName = name
            uses = n
            open()
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Remove from project?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 420
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 13
            text: "“" + removeDialog.assetName + "” is used in " + removeDialog.uses + (removeDialog.uses === 1 ? " clip" : " clips")
                  + " on the timeline. Remove them too? The original file is not deleted."
        }
        onAccepted: root.pool.removeAsset(assetId, true)
    }
}

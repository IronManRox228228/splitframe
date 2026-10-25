import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import SplitFrame

// Port of screens/ProjectListScreen.tsx: the Home page. Wordmark over a soft grey glow, a search pill with "New video", four format
// cards and "Start from footage", then the recent projects as cards. Dropping footage anywhere (the window's DropArea) starts a 16:9
// project with it.
Item {
    id: home

    // the window decides what starting a project means (unsaved changes first)
    signal createRequested(int width, int height, bool importAfter)
    signal openRequested(string path)

    property var recents: []
    property string query: ""
    property string renamingPath: ""
    readonly property var shown: {
        const q = query.trim().toLowerCase()
        return q === "" ? recents : recents.filter(p => String(p.name).toLowerCase().indexOf(q) >= 0)
    }

    readonly property var formats: [
        { title: "Short", hint: "9:16 · Reels, TikTok", w: 1080, h: 1920, bw: 28, bh: 50 },
        { title: "Video", hint: "16:9 · YouTube", w: 1920, h: 1080, bw: 72, bh: 40 },
        { title: "Square", hint: "1:1 · Feed posts", w: 1080, h: 1080, bw: 48, bh: 48 },
        { title: "Portrait", hint: "4:5 · Instagram", w: 1080, h: 1350, bw: 40, bh: 50 }
    ]

    function refresh() { recents = appEditor.homeProjects() }
    Component.onCompleted: refresh()
    Connections {
        target: appEditor.recent
        function onChanged() { home.refresh() }
    }
    onVisibleChanged: if (visible) refresh()

    Flickable {
        id: scroll
        anchors.fill: parent
        anchors.bottomMargin: 40
        contentWidth: width
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            width: Math.min(1120, scroll.width)
            x: (scroll.width - width) / 2
            spacing: 0

            // ---- wordmark + tagline over the glow
            Item {
                Layout.fillWidth: true
                Layout.topMargin: 64
                id: hero
                implicitHeight: heroCol.implicitHeight
                // the only gradient in the app: a soft grey glow behind the wordmark
                Shape {
                    id: glow
                    width: 560
                    height: 560
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -0.05 * 560
                    transform: Scale { origin.x: 280; origin.y: 280; xScale: 1100 / 560; yScale: 1 }
                    ShapePath {
                        strokeWidth: -1
                        fillGradient: RadialGradient {
                            centerX: 280; centerY: 280; centerRadius: 280
                            focalX: 280; focalY: 280
                            GradientStop { position: 0.0; color: "#3Deceeec" }
                            GradientStop { position: 0.4; color: "#1Aeceeec" }
                            GradientStop { position: 0.7; color: "#08eceeec" }
                            GradientStop { position: 1.0; color: "#00eceeec" }
                        }
                        PathRectangle { x: 0; y: 0; width: 560; height: 560; radius: 0 }
                    }
                }
                ColumnLayout {
                    id: heroCol
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 22
                    Image {
                        Layout.alignment: Qt.AlignHCenter
                        source: "assets/splitframe-logo.png"
                        sourceSize.width: 680
                        Layout.preferredWidth: 340
                        Layout.preferredHeight: 340 * 246 / 762
                        smooth: true
                        mipmap: true
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Tell it what the cut should feel like. Your footage never leaves this machine."
                        color: Theme.muted
                        font.pixelSize: 15
                    }
                }
            }

            // ---- search pill + New video
            GlassPanel {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 44
                Layout.preferredWidth: Math.min(560, content.width - 64)
                Layout.preferredHeight: 50
                radius: 25
                backdrop: hero
                border.color: searchField.activeFocus ? Theme.accentRing : Theme.glassBorder
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 8
                    spacing: 12
                    Icon { name: "search"; size: 17; color: Theme.muted }
                    TextField {
                        id: searchField
                        Layout.fillWidth: true
                        placeholderText: "Search projects"
                        placeholderTextColor: Theme.faint
                        color: Theme.text
                        font.pixelSize: 13
                        background: null
                        leftPadding: 0
                        selectByMouse: true
                        text: home.query
                        onTextChanged: home.query = text
                    }
                    IconBtn {
                        visible: home.query !== ""
                        small: true
                        iconName: "close"
                        iconSize: 14
                        onClicked: searchField.clear()
                    }
                    GButton {
                        text: "New video"
                        variant: "primary"
                        implicitHeight: 36
                        leftPadding: 18
                        rightPadding: 18
                        onClicked: home.createRequested(1920, 1080, false)
                    }
                }
            }

            // ---- format cards
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 44
                Layout.leftMargin: 32
                Layout.rightMargin: 32
                columns: 5
                columnSpacing: 14
                rowSpacing: 14
                Repeater {
                    model: home.formats
                    delegate: GlassPanel {
                        id: fcard
                        backdrop: hero
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 132
                        color: fArea.containsMouse ? "#12ffffff" : Theme.glassFill
                        border.color: fArea.containsMouse ? Theme.lineStrong : Theme.glassBorder
                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 12
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                implicitWidth: fcard.modelData.bw
                                implicitHeight: fcard.modelData.bh
                                radius: 6
                                color: "transparent"
                                border.width: 1.5
                                border.color: "#8Ceceeec"
                            }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Text { Layout.alignment: Qt.AlignHCenter; text: fcard.modelData.title; color: Theme.text; font.pixelSize: 13; font.weight: Font.Medium }
                                Text { Layout.alignment: Qt.AlignHCenter; text: fcard.modelData.hint; color: Theme.muted; font.pixelSize: 11 }
                            }
                        }
                        MouseArea {
                            id: fArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: home.createRequested(fcard.modelData.w, fcard.modelData.h, false)
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 132
                    DashedBox {
                        anchors.fill: parent
                        radius: 22
                        fillColor: "#59080A09"
                        borderColor: footArea.containsMouse ? Theme.muted : Theme.lineStrong
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 12
                        Icon { Layout.alignment: Qt.AlignHCenter; name: "upload"; size: 22; strokeWidth: 1.6; color: footArea.containsMouse ? Theme.text : Theme.fg2 }
                        ColumnLayout {
                            Layout.alignment: Qt.AlignHCenter
                            spacing: 2
                            Text { Layout.alignment: Qt.AlignHCenter; text: "Start from footage"; color: footArea.containsMouse ? Theme.text : Theme.fg2; font.pixelSize: 13; font.weight: Font.Medium }
                            Text { Layout.alignment: Qt.AlignHCenter; text: "Choose video, audio or photos"; color: Theme.muted; font.pixelSize: 11 }
                        }
                    }
                    MouseArea {
                        id: footArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: home.createRequested(1920, 1080, true)
                    }
                }
            }

            // ---- Recent
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 44
                Layout.leftMargin: 32
                Layout.rightMargin: 32
                spacing: 14
                Text { text: "Recent"; color: Theme.text; font.pixelSize: 15; font.weight: Font.DemiBold }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.line }
            }

            Item {
                visible: home.recents.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 14
                Layout.leftMargin: 32
                Layout.rightMargin: 32
                implicitHeight: 118
                DashedBox { anchors.fill: parent; radius: 22 }
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 8
                    Text { Layout.alignment: Qt.AlignHCenter; text: "No videos yet"; color: Theme.text; font.pixelSize: 14; font.weight: Font.Medium }
                    Text { Layout.alignment: Qt.AlignHCenter; text: "Pick a format above, or start from footage you already have."; color: Theme.muted; font.pixelSize: 12 }
                }
            }
            Text {
                visible: home.recents.length > 0 && home.shown.length === 0
                Layout.topMargin: 14
                Layout.leftMargin: 32
                Layout.bottomMargin: 24
                text: "No projects match “" + home.query.trim() + "”."
                color: Theme.muted
                font.pixelSize: 14
            }
            GridLayout {
                id: recentGrid
                readonly property real cellW: Math.floor((content.width - 64 - 3 * columnSpacing) / 4)
                visible: home.shown.length > 0
                Layout.fillWidth: true
                Layout.topMargin: 14
                Layout.leftMargin: 32
                Layout.rightMargin: 32
                Layout.bottomMargin: 64
                columns: 4
                columnSpacing: 16
                rowSpacing: 16
                Repeater {
                    model: home.shown
                    delegate: ProjectCard {
                        required property var modelData
                        Layout.fillWidth: false
                        Layout.preferredWidth: recentGrid.cellW
                        Layout.alignment: Qt.AlignTop
                        project: modelData
                        backdrop: hero
                        renaming: home.renamingPath === modelData.path
                        onOpenRequested: home.openRequested(modelData.path)
                        onRenameStarted: home.renamingPath = modelData.path
                        onRenameCancelled: home.renamingPath = ""
                        onRenameCommitted: name => {
                            home.renamingPath = ""
                            appEditor.renameProjectFile(modelData.path, name)
                            home.refresh()
                        }
                        onDuplicateRequested: { appEditor.duplicateProjectFile(modelData.path); home.refresh() }
                        onRevealRequested: appEditor.revealInFolder(modelData.path)
                        onRemoveRequested: appEditor.removeRecent(modelData.path)
                    }
                }
            }
        }
    }

    // ---- footer
    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 32
        anchors.rightMargin: 32
        height: 40
        Text { text: "SplitFrame v" + appEditor.appVersion(); color: Theme.faint; font.pixelSize: 12 }
        Item { Layout.fillWidth: true }
        Text { text: "Video engine: FFmpeg + GPU compositor"; color: Theme.faint; font.pixelSize: 12 }
    }
}

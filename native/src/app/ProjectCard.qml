import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import "format.js" as Fmt

// Port of home/ProjectCard.tsx: a glass card with a 16:10 picture (duration and aspect chips on it), a "more" menu that appears on
// hover (Rename, Duplicate, Show in folder, and Remove from list where the Electron app deleted the project folder), the name and
// "Edited ...". Opens the project on click.
GlassPanel {
    id: card
    property var project: ({})
    property bool renaming: false

    signal openRequested
    signal renameStarted
    signal renameCommitted(string name)
    signal renameCancelled
    signal duplicateRequested
    signal revealRequested
    signal removeRequested

    readonly property bool missing: project.exists === false
    implicitHeight: col.implicitHeight + 20

    HoverHandler { id: hover }

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 8
        anchors.bottomMargin: 12
        spacing: 10

        Item {
            id: thumbBox
            Layout.fillWidth: true
            Layout.preferredHeight: width * 10 / 16

            Item {
                id: maskItem
                anchors.fill: parent
                visible: false
                layer.enabled: true
                Rectangle { anchors.fill: parent; radius: 15 }
            }
            Rectangle {
                id: thumbFrame
                anchors.fill: parent
                radius: 15
                color: Theme.field
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: maskItem
                }

                Image {
                    id: thumb
                    anchors.fill: parent
                    visible: status === Image.Ready
                    source: card.project.thumb ? card.project.thumb : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    sourceSize.width: 640
                }
                Icon {
                    visible: thumb.status !== Image.Ready
                    anchors.centerIn: parent
                    name: "media"
                    size: 28
                    strokeWidth: 1.4
                    color: Theme.surface600
                }
            }
            // inset 1px ring (white .06, .18 on hover)
            Rectangle {
                anchors.fill: parent
                radius: 15
                color: "transparent"
                border.width: 1
                border.color: openArea.containsMouse ? "#2effffff" : "#0fffffff"
            }
            Rectangle {
                x: 8
                y: parent.height - height - 8
                height: 20
                width: Math.max(44, dur.implicitWidth + 18)
                radius: 10
                color: "#99050606"
                Text {
                    id: dur
                    anchors.centerIn: parent
                    text: Fmt.formatDuration(card.project.durationSec || 0)
                    color: Theme.text
                    font.family: Theme.mono
                    font.pixelSize: 11
                }
            }
            Rectangle {
                x: parent.width - width - 8
                y: parent.height - height - 8
                height: 20
                width: Math.max(40, asp.implicitWidth + 18)
                radius: 10
                color: "#99050606"
                Text {
                    id: asp
                    anchors.centerIn: parent
                    text: Fmt.aspectLabel(card.project.width || 0, card.project.height || 0)
                    color: Theme.fg2
                    font.pixelSize: 11
                }
            }
            MouseArea {
                id: openArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: !card.missing
                cursorShape: card.missing ? Qt.ArrowCursor : Qt.PointingHandCursor
                onClicked: card.openRequested()
            }
            Rectangle {
                id: moreBtn
                x: parent.width - width - 8
                y: 8
                width: 30
                height: 30
                radius: 15
                color: moreArea.containsMouse ? "#050606" : "#b3050606"
                opacity: hover.hovered || menu.visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 120 } }
                Icon { anchors.centerIn: parent; name: "more"; size: 16; color: Theme.text }
                MouseArea {
                    id: moreArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: menu.popup(moreBtn, moreBtn.width - menu.implicitWidth, moreBtn.height + 6)
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            spacing: 2
            Text {
                visible: !card.renaming
                Layout.fillWidth: true
                text: card.project.name || ""
                color: card.missing ? Theme.faint : Theme.text
                font.pixelSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
                MouseArea {
                    anchors.fill: parent
                    enabled: !card.missing
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.openRequested()
                }
            }
            GInput {
                id: renameField
                visible: card.renaming
                Layout.fillWidth: true
                fieldHeight: 28
                leftPadding: 8
                font.pixelSize: 14
                font.weight: Font.Medium
                maximumLength: 120
                property bool done: false
                onVisibleChanged: if (visible) {
                    done = false
                    text = card.project.name || ""
                    forceActiveFocus()
                    selectAll()
                }
                function finish(commit) {
                    if (done) return
                    done = true
                    const v = text.trim()
                    if (commit && v !== "" && v !== card.project.name) card.renameCommitted(v)
                    else card.renameCancelled()
                }
                Keys.onReturnPressed: finish(true)
                Keys.onEnterPressed: finish(true)
                Keys.onEscapePressed: finish(false)
                onActiveFocusChanged: if (!activeFocus && visible) finish(true)
            }
            Text {
                Layout.fillWidth: true
                text: card.missing ? "File missing" : Fmt.editedLabel(card.project.modified || "")
                color: card.missing ? Theme.danger : Theme.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }

    GlassMenu {
        id: menu
        items: card.missing
               ? [{ label: "Remove from list", danger: true }]
               : [{ label: "Rename" }, { label: "Duplicate" }, { label: "Show in folder" }, { label: "Remove from list", danger: true, separatorBefore: true }]
        onPicked: index => {
            const label = items[index].label
            if (label === "Rename") card.renameStarted()
            else if (label === "Duplicate") card.duplicateRequested()
            else if (label === "Show in folder") card.revealRequested()
            else card.removeRequested()
        }
    }
}

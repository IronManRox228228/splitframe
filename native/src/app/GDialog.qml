import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Electron app's Dialog (ui/Dialog.tsx): a modal glass-strong slab (22px corners) with a 17px title and a close button over a
// black/60 scrim. Escape and a click outside close it unless `locked`. Put the body in as children; they sit in a column that is as
// wide as the dialog (use Layout.fillWidth), and it scrolls when the window is too short.
Popup {
    id: dlg
    property string title: ""
    property real dialogWidth: 520
    property bool locked: false
    default property alias body: bodyColumn.data

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    padding: 0
    closePolicy: locked ? Popup.NoAutoClose : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
    width: Math.min(dialogWidth, parent ? parent.width - 48 : dialogWidth)
    height: Math.min(header.height + bodyColumn.implicitHeight + 4, parent ? parent.height - 48 : 600)

    // Escape belongs to the dialog while one is open (the window's Deselect shortcut stands down)
    onOpened: Theme.modalDepth++
    onClosed: Theme.modalDepth = Math.max(0, Theme.modalDepth - 1)

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 } }
    Overlay.modal: Rectangle { color: Theme.scrim }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: dlgGlass.active ? "transparent" : Theme.glassStrong
        border.width: 1
        border.color: Theme.glassBorder
        RefractiveGlass { id: dlgGlass; armed: dlg.visible; anchors.fill: parent; radius: parent.radius; backdrop: Theme.backdropItem; strength: 18; tint: Qt.rgba(0.063, 0.07, 0.067, 0.94); blurRadius: 5 }
        Rectangle { x: parent.radius * 0.6; y: 1; width: parent.width - parent.radius * 1.2; height: 1; color: Theme.glassHighlight }
    }

    contentItem: Item {
        RowLayout {
            id: header
            x: 0
            y: 0
            width: parent.width
            height: 52
            Text {
                Layout.leftMargin: 24
                Layout.topMargin: 10
                Layout.fillWidth: true
                text: dlg.title
                color: Theme.text
                font.pixelSize: 17
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            IconBtn {
                visible: !dlg.locked
                Layout.rightMargin: 12
                Layout.topMargin: 10
                small: true
                iconName: "close"
                iconSize: 16
                tip: "Close"
                onClicked: dlg.close()
            }
        }
        Flickable {
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            contentWidth: width
            contentHeight: bodyColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: bodyColumn
                width: parent.width
                spacing: 0
            }
        }
    }
}

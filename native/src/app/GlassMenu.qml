import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Electron app's floating menu (ui/Menu.tsx): glass-strong, 16px corners, 6px padding, 32px rows. `items` is
// [{label, danger?, disabled?, separatorBefore?, hint?}]; picking one emits picked(index) and closes. Open it with popup(item) or
// by setting x / y and calling open().
Popup {
    id: menu
    property var items: []
    property real minWidth: 176
    signal picked(int index)

    padding: 6
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    implicitWidth: Math.max(minWidth, column.implicitWidth + 12)
    implicitHeight: column.implicitHeight + 12

    onOpened: Theme.modalDepth++
    onClosed: Theme.modalDepth = Math.max(0, Theme.modalDepth - 1)
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }

    background: Rectangle {
        radius: 16
        color: menuGlass.active ? "transparent" : Theme.glassStrong
        border.width: 1
        border.color: Theme.glassBorder
        RefractiveGlass { id: menuGlass; armed: menu.visible; anchors.fill: parent; radius: parent.radius; backdrop: Theme.backdropItem; strength: 12; bevel: 14; tint: Qt.rgba(0.063, 0.07, 0.067, 0.94); blurRadius: 5 }
        Rectangle { x: parent.radius * 0.6; y: 1; width: parent.width - parent.radius * 1.2; height: 1; color: Theme.glassHighlight }
    }

    contentItem: ColumnLayout {
        id: column
        spacing: 0
        Repeater {
            model: menu.items
            delegate: ColumnLayout {
                id: entry
                required property int index
                required property var modelData
                Layout.fillWidth: true
                spacing: 0
                Rectangle {
                    visible: entry.modelData.separatorBefore === true
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    implicitHeight: 1
                    color: Theme.line
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 32
                    implicitWidth: label.implicitWidth + 24
                    radius: 12
                    color: area.containsMouse && entry.modelData.disabled !== true ? "#14ffffff" : "transparent"
                    opacity: entry.modelData.disabled === true ? 0.4 : 1
                    Text {
                        id: label
                        anchors.verticalCenter: parent.verticalCenter
                        x: 10
                        text: entry.modelData.label
                        font.pixelSize: 13
                        color: entry.modelData.danger === true ? Theme.danger : (area.containsMouse ? Theme.text : Theme.fg2)
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        enabled: entry.modelData.disabled !== true
                        onClicked: {
                            menu.close()
                            menu.picked(entry.index)
                        }
                    }
                }
            }
        }
    }
}

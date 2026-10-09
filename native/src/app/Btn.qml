import QtQuick
import QtQuick.Controls

// Flat toolbar button. Never takes keyboard focus, so Space and the arrow keys keep driving the editor.
Button {
    id: control
    property bool active: false
    property string tip: ""
    property string glyph: ""
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    implicitHeight: 28
    implicitWidth: Math.max(30, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: 10
    rightPadding: 10
    contentItem: Row {
        spacing: 6
        Text {
            visible: control.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.glyph
            font.family: Theme.icons
            font.pixelSize: 14
            color: control.enabled ? Theme.text : Theme.faint
        }
        Text {
            visible: control.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.pixelSize: 12
            color: control.enabled ? Theme.text : Theme.faint
            elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: 5
        color: control.down ? Theme.pressed : (control.active || control.checked ? Theme.selected : (control.hovered && control.enabled ? Theme.hover : Theme.raised))
        border.color: control.active || control.checked ? Theme.accent : Theme.lineStrong
        opacity: control.enabled ? 1 : 0.55
    }
    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}

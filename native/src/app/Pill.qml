import QtQuick
import QtQuick.Controls

// A fully rounded toggle chip: the .pill / .pill-active of the Electron app. Never takes keyboard focus.
AbstractButton {
    id: control
    property bool active: checked
    property color tint: Theme.text
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    implicitHeight: 26
    implicitWidth: label.implicitWidth + 24
    contentItem: Text {
        id: label
        text: control.text
        font.pixelSize: 12
        font.weight: control.active ? Font.Medium : Font.Normal
        color: !control.enabled ? Theme.faint : (control.active ? "#0B0D0C" : (control.hovered ? Theme.text : Theme.fg2))
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: Theme.pill
        color: control.active ? control.tint : (control.down ? Theme.pressed : "transparent")
        border.width: 1
        border.color: control.active ? control.tint : (control.hovered ? Theme.lineStrong : Theme.line)
        opacity: control.enabled ? 1 : 0.5
    }
}

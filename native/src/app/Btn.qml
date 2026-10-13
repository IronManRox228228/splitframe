import QtQuick
import QtQuick.Controls

// Pill toolbar button (.btn-outline / .btn-ghost of the Electron app). Never takes keyboard focus, so Space and the
// arrow keys keep driving the editor. `accentFill` makes it the one primary action of a view.
Button {
    id: control
    property bool active: false
    property bool accentFill: false
    property string tip: ""
    property string glyph: ""
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    implicitHeight: 28
    implicitWidth: Math.max(30, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: 12
    rightPadding: 12
    contentItem: Row {
        spacing: 6
        Text {
            visible: control.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.glyph
            font.family: Theme.icons
            font.pixelSize: 14
            color: control.enabled ? (control.accentFill ? "#0B0D0C" : Theme.text) : Theme.faint
        }
        Text {
            visible: control.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.pixelSize: 12
            font.weight: control.accentFill ? Font.DemiBold : Font.Normal
            color: control.enabled ? (control.accentFill ? "#0B0D0C" : Theme.text) : Theme.faint
            elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: Theme.pill
        color: control.accentFill ? (control.down ? Theme.accentDeep : (control.hovered ? Theme.accentHover : Theme.accent))
                                  : (control.down ? Theme.pressed : (control.active || control.checked ? Theme.hover : (control.hovered && control.enabled ? "#12ffffff" : "transparent")))
        border.width: control.accentFill ? 0 : 1
        border.color: control.active || control.checked ? Theme.accent : (control.hovered && control.enabled ? Theme.lineStrong : Theme.line)
        opacity: control.enabled ? 1 : 0.5
    }
    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}

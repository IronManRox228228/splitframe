import QtQuick
import QtQuick.Controls

// The Electron app's .icon-btn: a round, quiet icon button (36px, or 32px with `small`). Never takes keyboard focus.
AbstractButton {
    id: control
    property string iconName: ""
    property bool small: false
    property real iconSize: 16
    property string tip: ""
    property color tint: hovered ? Theme.text : Theme.muted

    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    implicitWidth: small ? 32 : 36
    implicitHeight: implicitWidth

    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: control.iconSize
            color: control.tint
        }
    }
    background: Rectangle {
        radius: width / 2
        color: control.down ? Theme.pressWash : (control.hovered && control.enabled ? Theme.hoverWash : (control.checked ? Theme.accentSoft : "transparent"))
        opacity: control.enabled ? 1 : 0.3
    }
    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}

import QtQuick
import QtQuick.Controls

// The Electron app's .btn family (styles.css): primary, accent, secondary, ghost, outline and danger pills, normal (36px) or
// small (32px, .btn-sm). Never takes keyboard focus, so Space and the arrow keys keep driving the editor.
AbstractButton {
    id: control
    property string variant: "outline" // primary | accent | secondary | ghost | outline | danger
    property bool small: false
    property string iconName: ""
    property string tip: ""

    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    implicitHeight: small ? 32 : 36
    implicitWidth: row.implicitWidth + leftPadding + rightPadding
    leftPadding: small ? 14 : 16
    rightPadding: small ? 14 : 16

    readonly property bool filled: variant === "primary" || variant === "accent"
    readonly property color fg: !enabled ? Theme.fg2
        : variant === "primary" || variant === "accent" ? Theme.ink
        : variant === "danger" ? Theme.danger
        : variant === "secondary" ? Theme.text
        : (hovered ? Theme.text : Theme.fg2)

    contentItem: Row {
        id: row
        spacing: 6
        Icon {
            visible: control.iconName !== ""
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            size: control.small ? 14 : 16
            color: control.fg
        }
        Text {
            visible: control.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font.pixelSize: control.small ? 12 : 13
            font.weight: control.filled ? Font.DemiBold : Font.Medium
            color: control.fg
        }
    }

    background: Rectangle {
        radius: Theme.pill
        opacity: control.enabled ? 1 : 0.4
        color: {
            switch (control.variant) {
            case "primary": return control.hovered ? "#FFFFFF" : Theme.text
            case "accent": return control.hovered ? Theme.accentHover : Theme.accent
            case "secondary": return Theme.btnSecondaryFill
            case "ghost": return control.hovered ? Theme.hoverWash : "transparent"
            case "danger": return control.hovered ? "#40F28B82" : Theme.dangerWash
            default: return "transparent"
            }
        }
        border.width: control.variant === "secondary" || control.variant === "outline" ? 1 : 0
        border.color: control.hovered ? Theme.text : Theme.lineStrong
    }

    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}

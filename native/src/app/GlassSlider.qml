import QtQuick
import QtQuick.Controls

// Horizontal slider: a hairline track, mint fill and a round thumb. `moved` fires while dragging, `released` on let go.
Slider {
    id: control
    signal released
    focusPolicy: Qt.NoFocus
    implicitHeight: 22
    onPressedChanged: if (!pressed) released()
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.line
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.enabled ? Theme.accent : Theme.faint
        }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 14
        height: 14
        radius: 7
        color: control.pressed ? Theme.accentHover : Theme.text
        border.width: 2
        border.color: control.enabled ? Theme.accent : Theme.faint
    }
}

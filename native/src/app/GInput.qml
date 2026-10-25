import QtQuick
import QtQuick.Controls

// The Electron app's .input: a 36px rounded field on rgba(8,10,9,.4) with a hairline border that turns mint when focused.
TextField {
    id: field
    property bool pill: false
    property real fieldHeight: 36

    color: Theme.text
    placeholderTextColor: Theme.faint
    selectionColor: Theme.accentDeep
    selectedTextColor: Theme.text
    font.pixelSize: 13
    selectByMouse: true
    leftPadding: 12
    rightPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    implicitHeight: fieldHeight
    background: Rectangle {
        radius: field.pill ? height / 2 : 12
        color: Theme.inputFill
        border.width: 1
        border.color: field.activeFocus ? Theme.accentRing : Theme.line
    }
}

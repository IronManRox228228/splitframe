import QtQuick
import QtQuick.Controls

// Drop-down in the glass look. `placeholder` shows while currentIndex is -1.
ComboBox {
    id: control
    property string placeholder: ""
    focusPolicy: Qt.NoFocus
    implicitHeight: 30
    font.pixelSize: 12
    contentItem: Text {
        leftPadding: 12
        rightPadding: 28
        text: control.currentIndex >= 0 ? control.displayText : control.placeholder
        font: control.font
        color: control.currentIndex >= 0 ? Theme.text : Theme.faint
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideMiddle
    }
    indicator: Text {
        x: control.width - width - 12
        y: (control.height - height) / 2
        text: "\u25BE"
        color: Theme.muted
        font.pixelSize: 11
    }
    background: Rectangle {
        radius: Theme.radiusField
        color: Theme.field
        border.width: 1
        border.color: control.popup.visible ? Theme.accent : (control.hovered ? Theme.lineStrong : Theme.line)
        opacity: control.enabled ? 1 : 0.5
    }
    delegate: ItemDelegate {
        width: control.width
        height: 28
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textAt(index)
            color: Theme.text
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideMiddle
        }
        background: Rectangle {
            radius: 6
            color: parent.highlighted ? Theme.hover : "transparent"
        }
    }
    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 320)
        padding: 6
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            radius: 14
            color: "#f2101211"
            border.width: 1
            border.color: Theme.glassBorder
        }
    }
}

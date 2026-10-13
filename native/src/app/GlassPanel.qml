import QtQuick

// A glass slab: translucent fill over the ground, a 1px border and an inset highlight along the top edge.
// A translucent fill stands in for the Electron app's backdrop blur, which is not worth a blur pass per panel.
Rectangle {
    id: slab
    property bool strong: false
    color: strong ? Theme.glassStrong : Theme.glassFill
    radius: Theme.radiusPanel
    border.width: 1
    border.color: Theme.glassBorder

    Rectangle {
        // inset top highlight
        x: slab.radius * 0.6
        y: 1
        width: slab.width - slab.radius * 1.2
        height: 1
        color: Theme.glassHighlight
    }
}

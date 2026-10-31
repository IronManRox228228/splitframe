import QtQuick

// A glass slab: translucent fill over the ground, a 1px border and an inset highlight along the top edge.
// Flat by default (a translucent fill stands in for the Electron app's backdrop blur, which is not worth a blur pass per panel).
// Give it a `backdrop` (an item behind it, not containing it) and it refracts that instead: RefractiveGlass bends and softens the
// picture near the rim. It falls back to the flat fill whenever the shader is unavailable or Theme.refractionOn is off.
Rectangle {
    id: slab
    property bool strong: false
    property Item backdrop: null
    // re-capture the backdrop whenever it changes (e.g. playing video behind the slab) instead of on refreshBackdrop() only
    property bool liveBackdrop: false
    readonly property bool refracting: fx.active
    function refreshBackdrop() { fx.refresh() }

    color: refracting ? "transparent" : (strong ? Theme.glassStrong : Theme.glassFill)
    radius: Theme.radiusPanel
    border.width: 1
    border.color: Theme.glassBorder

    RefractiveGlass {
        id: fx
        anchors.fill: parent
        z: -1
        backdrop: slab.backdrop
        live: slab.liveBackdrop
        radius: slab.radius
        strength: slab.strong ? 16 : 10
        bevel: Math.min(22, slab.radius)
        tint: slab.strong ? Qt.rgba(0.063, 0.07, 0.067, 0.86) : Qt.rgba(1, 1, 1, 0.06)
    }

    Rectangle {
        // inset top highlight
        x: slab.radius * 0.6
        y: 1
        width: slab.width - slab.radius * 1.2
        height: 1
        color: Theme.glassHighlight
    }
}

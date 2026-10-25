import QtQuick

// Refraction glass: a rounded slab that bends, softens and fringes whatever `backdrop` shows behind it (see shaders/refract.frag), with
// a specular rim. `backdrop` is an item that does NOT contain this one. It is captured once into a small texture (live: false), so a
// static backdrop costs one render pass; call refresh() when it changes. Where the shader cannot load, Theme.refractionOn is off, or no
// backdrop is given, `active` is false and nothing is drawn: the parent keeps its flat glass fill.
Item {
    id: glass
    property Item backdrop: null
    property real radius: 22
    property real strength: 16
    property real bevel: 20
    property real blurRadius: 3
    property real aberration: 1.8
    property color tint: Qt.rgba(0.063, 0.07, 0.067, 0.72)
    property color rim: Qt.rgba(1, 1, 1, 0.55)
    property bool live: false
    // popups arm their glass only while open: an idle slab must not hold the window content as a render source
    property bool armed: true
    readonly property real margin: 24
    readonly property bool active: armed && Theme.refractionOn && backdrop !== null && width > 8 && height > 8 && effect.status !== ShaderEffect.Error

    function refresh() {
        if (!active || !backdrop) return
        const o = backdrop.mapFromItem(glass, -margin, -margin)
        capture.sourceRect = Qt.rect(o.x, o.y, width + 2 * margin, height + 2 * margin)
        capture.scheduleUpdate()
    }
    onActiveChanged: Qt.callLater(refresh)
    onWidthChanged: Qt.callLater(refresh)
    onHeightChanged: Qt.callLater(refresh)
    onVisibleChanged: if (visible) Qt.callLater(refresh)
    Component.onCompleted: Qt.callLater(refresh)

    ShaderEffectSource {
        id: capture
        visible: false
        sourceItem: glass.active ? glass.backdrop : null
        live: glass.live
        hideSource: false
        smooth: true
        wrapMode: ShaderEffectSource.ClampToEdge
        textureSize: Qt.size(Math.max(1, Math.ceil(glass.width + 2 * glass.margin)), Math.max(1, Math.ceil(glass.height + 2 * glass.margin)))
    }

    ShaderEffect {
        id: effect
        anchors.fill: parent
        visible: glass.active
        property variant src: capture
        property size glassSize: Qt.size(glass.width, glass.height)
        property size srcSize: Qt.size(glass.width + 2 * glass.margin, glass.height + 2 * glass.margin)
        property real margin: glass.margin
        property real radius: glass.radius
        property real bevel: glass.bevel
        property real strength: glass.strength
        property real blur: glass.blurRadius
        property real aberration: glass.aberration
        property color tint: glass.tint
        property color rim: glass.rim
        fragmentShader: "qrc:/qt/qml/SplitFrame/shaders/refract.frag.qsb"
    }
}

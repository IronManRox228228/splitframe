pragma Singleton
import QtQuick

// SplitFrame "clean glass": the Electron app's tokens (apps/desktop/tailwind.config.js, styles.css).
// A near-black ground, translucent glass slabs, one mint accent for live state.
QtObject {
    // ground and surfaces
    readonly property color bg: "#050606"
    readonly property color panel: "#0A0C0B"
    readonly property color panelAlt: "#0A0C0B"
    readonly property color field: "#0D0F0E"
    readonly property color raised: "#0D0F0E"
    readonly property color hover: "#1B1F1D"
    readonly property color pressed: "#343A37"
    readonly property color selected: "#1B1F1D"
    // lines: rgba(236,238,236,.10) and .22
    readonly property color line: "#1aeceeec"
    readonly property color lineStrong: "#38eceeec"
    readonly property color border: "#343A37"
    // glass slab: fill rgba(255,255,255,.045), border .09, inset top highlight .10
    readonly property color glassFill: "#0bffffff"
    readonly property color glassBorder: "#17ffffff"
    readonly property color glassHighlight: "#1affffff"
    readonly property color glassStrong: "#eb101211"
    // text
    readonly property color text: "#ECEEEC"
    readonly property color fg2: "#C4CAC7"
    readonly property color muted: "#8E9793"
    readonly property color faint: "#78837E"
    // accent and status
    readonly property color accent: "#5FB7A1"
    readonly property color accentHover: "#7BC9B5"
    readonly property color accentDeep: "#215C4F"
    readonly property color danger: "#F28B82"
    readonly property color success: "#86D39B"
    readonly property color warn: "#F2C46B"
    // Electron app extras (tailwind.config.js / styles.css): inputs and cards sit on glass as rgba(8,10,9,.4)
    readonly property color inputFill: "#66080A09"
    readonly property color cardFill: "#66080A09"
    readonly property color btnSecondaryFill: "#8C080A09"
    readonly property color hoverWash: "#12ffffff"   // white/[0.07]
    readonly property color pressWash: "#1Affffff"
    readonly property color accentRing: "#995FB7A1"  // accent/60 focus border
    readonly property color accentSoft: "#595FB7A1"  // accent-deep/35 tint
    readonly property color dangerSoft: "#1AF28B82"  // danger/10
    readonly property color dangerWash: "#26F28B82"  // danger/15
    readonly property color dangerLine: "#4DF28B82"  // danger/30
    readonly property color dangerLineStrong: "#66F28B82"
    readonly property color scrim: "#99000000"        // dialog backdrop: black/60
    readonly property color kbdFill: "#14ffffff"
    readonly property color ink: "#0B0D0C"            // text on light / accent fills
    readonly property color surface600: "#4A514E"
    // the agent crew (the Electron app had one assistant; the native crew has four, one hue each, all muted to sit on the mint glass)
    readonly property color agentDirector: "#5FB7A1"
    readonly property color agentEditor: "#8FB4F0"
    readonly property color agentColourist: "#E9A97C"
    readonly property color agentAudio: "#C2A5F0"
    function agentColor(name) {
        switch (String(name).toLowerCase()) {
        case "director": return agentDirector
        case "editor": return agentEditor
        case "colourist": case "colorist": return agentColourist
        case "audio": return agentAudio
        }
        return fg2
    }
    function agentLabel(name) {
        const n = String(name)
        return n.length ? n.charAt(0).toUpperCase() + n.slice(1) : "Agent"
    }
    // refraction glass: "refract" (default) or "flat"; the window sets it from --glass and the Settings
    property bool refractionOn: true
    // the item dialogs and menus refract: the window's content (set by Main.qml)
    property Item backdropItem: null
    // open GDialogs (Escape belongs to them while > 0)
    property int modalDepth: 0
    // type
    readonly property string sans: "Geist"
    readonly property string mono: "Geist Mono"
    readonly property string icons: "Segoe MDL2 Assets"
    // shape
    readonly property int radius: 8
    readonly property int radiusField: 10
    readonly property int radiusPanel: 22
    readonly property int pill: 999
    readonly property int gap: 8
}

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

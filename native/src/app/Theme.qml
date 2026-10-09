pragma Singleton
import QtQuick

// Colours and sizes shared by every panel. Same dark palette as the first preview window.
QtObject {
    readonly property color bg: "#0e0f12"
    readonly property color panel: "#14161a"
    readonly property color panelAlt: "#101211"
    readonly property color field: "#0b0c0f"
    readonly property color raised: "#1b1e25"
    readonly property color hover: "#262a33"
    readonly property color pressed: "#3a3f4a"
    readonly property color selected: "#2c3340"
    readonly property color line: "#23262d"
    readonly property color lineStrong: "#2c303a"
    readonly property color text: "#e8e8ea"
    readonly property color muted: "#a0a4ad"
    readonly property color faint: "#8b909a"
    readonly property color accent: "#5FB7A1"
    readonly property color accentDeep: "#2c5a4e"
    readonly property color danger: "#f08a8a"
    readonly property color warn: "#fbbf24"
    readonly property string mono: "Consolas"
    readonly property string icons: "Segoe MDL2 Assets"
    readonly property int radius: 6
}

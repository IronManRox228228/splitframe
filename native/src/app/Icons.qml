pragma Singleton
import QtQuick

// The Electron app's stroke icons (apps/desktop/src/renderer/ui/Icon.tsx), drawn on the same 24px grid. Each icon is a list of
// {d: SVG path, fill: bool}; circles and rounded rects of the original are written out as arcs. Icon.qml draws them.
QtObject {
    function circle(cx, cy, r) {
        return "M" + (cx - r) + " " + cy + "A" + r + " " + r + " 0 1 0 " + (cx + r) + " " + cy + "A" + r + " " + r + " 0 1 0 " + (cx - r) + " " + cy + "Z"
    }
    function rect(x, y, w, h, r) {
        return "M" + (x + r) + " " + y + "H" + (x + w - r) + "A" + r + " " + r + " 0 0 1 " + (x + w) + " " + (y + r)
            + "V" + (y + h - r) + "A" + r + " " + r + " 0 0 1 " + (x + w - r) + " " + (y + h)
            + "H" + (x + r) + "A" + r + " " + r + " 0 0 1 " + x + " " + (y + h - r)
            + "V" + (y + r) + "A" + r + " " + r + " 0 0 1 " + (x + r) + " " + y + "Z"
    }
    function s(d) { return { d: d, fill: false } }
    function f(d) { return { d: d, fill: true } }

    readonly property var table: ({
        back: [s("M15 18l-6-6 6-6")],
        chevronRight: [s("M9 18l6-6-6-6")],
        chevronDown: [s("M6 9l6 6 6-6")],
        undo: [s("M9 14L4 9l5-5"), s("M4 9h11a5 5 0 010 10h-3")],
        redo: [s("M15 14l5-5-5-5"), s("M20 9H9a5 5 0 000 10h3")],
        play: [f("M7 4.5v15l13-7.5z")],
        media: [s(rect(3, 5, 18, 14, 3)), s("M10 9.5v5l4-2.5z")],
        image: [s(rect(3, 4, 18, 16, 3)), s(circle(9, 10, 1.6)), s("M21 16l-5-5-8 9")],
        text: [s("M5 6h14"), s("M12 6v13")],
        audio: [s("M9 18V6l10-2v12"), s(circle(6.5, 18, 2.5)), s(circle(16.5, 16, 2.5))],
        captions: [s(rect(3, 5, 18, 14, 3)), s("M7 15h4"), s("M13 15h4"), s("M7 11h10")],
        sparkles: [s("M12 3l1.8 4.7L18.5 9.5l-4.7 1.8L12 16l-1.8-4.7L5.5 9.5l4.7-1.8z"), s("M19 15l.8 2.2L22 18l-2.2.8L19 21l-.8-2.2L16 18l2.2-.8z")],
        search: [s(circle(11, 11, 7)), s("M20 20l-3.5-3.5")],
        plus: [s("M12 5v14"), s("M5 12h14")],
        minus: [s("M5 12h14")],
        upload: [s("M12 16V4"), s("M7 9l5-5 5 5"), s("M4 16v3a2 2 0 002 2h12a2 2 0 002-2v-3")],
        close: [s("M6 6l12 12"), s("M18 6L6 18")],
        more: [f(circle(5, 12, 1.6)), f(circle(12, 12, 1.6)), f(circle(19, 12, 1.6))],
        trash: [s("M4 7h16"), s("M10 11v6"), s("M14 11v6"), s("M6 7l1 12a2 2 0 002 2h6a2 2 0 002-2l1-12"), s("M9 7V4h6v3")],
        folder: [s("M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z")],
        check: [s("M5 12.5l4.5 4.5L19 7.5")],
        alert: [s(circle(12, 12, 9)), s("M12 7.5v5.5"), s("M12 16.5v.01")],
        settings: [s("M4 7h10"), s("M18 7h2"), s(circle(16, 7, 2)), s("M4 17h2"), s("M10 17h10"), s(circle(8, 17, 2))],
        copy: [s(rect(8, 8, 12, 12, 2.5)), s("M16 8V6a2 2 0 00-2-2H6a2 2 0 00-2 2v8a2 2 0 002 2h2")],
        retry: [s("M20 11a8 8 0 10-2.3 5.7"), s("M20 5v6h-6")],
        send: [s("M12 19V5"), s("M5 12l7-7 7 7")],
        stop: [f(rect(6.5, 6.5, 11, 11, 2))],
        layers: [s("M12 3l9 5-9 5-9-5z"), s("M3 13l9 5 9-5")],
        plug: [s("M9 3v5"), s("M15 3v5"), s("M6 8h12v3a6 6 0 01-12 0z"), s("M12 17v4")],
        film: [s(rect(3, 4, 18, 16, 3)), s("M3 9h18"), s("M3 15h18"), s("M8 4v16"), s("M16 4v16")],
        external: [s("M14 4h6v6"), s("M20 4l-9 9"), s("M18 14v4a2 2 0 01-2 2H6a2 2 0 01-2-2V8a2 2 0 012-2h4")]
    })

    function get(name) {
        const e = table[name]
        return e === undefined ? [] : e
    }
}

.pragma library

// Port of apps/desktop/src/renderer/home/format.ts: small display helpers for the Home cards.

function formatDuration(sec) {
    const total = Math.max(0, Math.round(sec))
    const h = Math.floor(total / 3600)
    const m = Math.floor((total % 3600) / 60)
    const s = total % 60
    const ss = (s < 10 ? "0" : "") + s
    return h > 0 ? h + ":" + (m < 10 ? "0" : "") + m + ":" + ss : m + ":" + ss
}

const KNOWN_RATIOS = [["9:16", 9 / 16], ["16:9", 16 / 9], ["1:1", 1], ["4:5", 4 / 5], ["5:4", 5 / 4], ["3:4", 3 / 4], ["4:3", 4 / 3], ["21:9", 21 / 9]]

function gcd(a, b) { return b === 0 ? a : gcd(b, a % b) }

function aspectLabel(width, height) {
    if (width <= 0 || height <= 0) return "16:9"
    const r = width / height
    for (let i = 0; i < KNOWN_RATIOS.length; ++i)
        if (Math.abs(KNOWN_RATIOS[i][1] - r) / KNOWN_RATIOS[i][1] < 0.02) return KNOWN_RATIOS[i][0]
    const d = gcd(width, height)
    const w = width / d
    const h = height / d
    return w <= 32 && h <= 32 ? w + ":" + h : r.toFixed(2) + ":1"
}

const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]

// "Edited 12 min ago", "Edited 2 h ago", "Edited yesterday", "Edited Oct 2".
function editedLabel(iso) {
    const then = new Date(iso)
    if (isNaN(then.getTime())) return "Edited"
    const now = new Date()
    const min = Math.floor((now.getTime() - then.getTime()) / 60000)
    if (min < 1) return "Edited just now"
    if (min < 60) return "Edited " + min + " min ago"
    const startOfDay = function (d) { return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() }
    const days = Math.round((startOfDay(now) - startOfDay(then)) / 86400000)
    if (days === 0) return "Edited " + Math.floor(min / 60) + " h ago"
    if (days === 1) return "Edited yesterday"
    const base = MONTHS[then.getMonth()] + " " + then.getDate()
    return then.getFullYear() === now.getFullYear() ? "Edited " + base : "Edited " + base + ", " + then.getFullYear()
}

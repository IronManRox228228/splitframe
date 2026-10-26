.pragma library

// Display rules for the Assistant's cards: ports of TOOL_LABELS, summarizeResult and friendlyError from editor/ChatTab.tsx, for the
// native crew (engine command names such as "timeline.get", errors as provider codes).

const LABELS = {
    "timeline.get": "Read timeline",
    "project.info": "Read project",
    "media.list": "List footage",
    "media.get": "Read file details",
    "media.transcript": "Read transcript",
    "transcript.get": "Read transcript",
    "transcript.search": "Search transcript",
    "frame.capture": "Look at frame",
    "clip.add": "Add clip",
    "item.add": "Add clip",
    "item.update": "Change clip",
    "item.move": "Move clip",
    "item.trim": "Trim clip",
    "item.split": "Split clip",
    "item.clone": "Duplicate clip",
    "item.delete": "Delete clips",
    "marker.add": "Add marker",
    "captions.add": "Add captions",
    "silences.remove": "Remove silences",
    "assemble.roughcut": "Build rough cut",
    "audio.duck": "Duck music",
    "beat.sync": "Beat sync",
    "title.add": "Add title",
    "export.start": "Export",
    "session.undo": "Undo",
    "session.redo": "Redo",
    "storyboard.get": "Read storyboard",
    "storyboard.set": "Write storyboard",
    "storyboard.scene.add": "Add storyboard scene",
    "agent.assign": "Hand off to a specialist",
    "peer_request": "Ask a colleague",
    "generate.shot": "Generate a shot",
    "generate.video": "Generate video",
    "generate.image": "Generate image",
    "generate.speech": "Generate speech",
    "generate.music": "Generate music"
}

function toolLabel(name) {
    const n = String(name || "")
    if (LABELS[n]) return LABELS[n]
    if (n === "") return "Tool"
    const words = n.replace(/[_]+/g, " ").split(".").join(" ")
    return words.charAt(0).toUpperCase() + words.slice(1)
}

function plural(n, one, many) { return n + " " + (n === 1 ? one : (many || one + "s")) }

// parse a result's text: the crew sends tool results as compact JSON
function parseResult(text) {
    if (text === undefined || text === null || text === "") return null
    try { return JSON.parse(text) } catch (e) { return String(text) }
}

// A failure's reason, or "" when the tool worked.
function failureOf(result, ok) {
    if (result && typeof result === "object") {
        const e = result.error
        if (typeof e === "string" && e) return e
        if (e && typeof e === "object") return String(e.message || e.code || "failed")
        if (result.ok === false) return String(result.reason || result.message || "The step failed")
    }
    return ok === false ? (typeof result === "string" && result ? result : "The step failed") : ""
}

// One short line saying what a tool did, from whatever its result contains.
function summarize(result) {
    if (result === null || result === undefined) return ""
    if (Array.isArray(result)) return plural(result.length, "result")
    if (typeof result !== "object") return String(result).slice(0, 80)
    const keys = ["removedCount", "removed", "added", "captionCount", "cuts", "created"]
    for (let i = 0; i < keys.length; ++i)
        if (typeof result[keys[i]] === "number") return keys[i].replace(/Count$/, "") + ": " + result[keys[i]]
    if (Array.isArray(result.scenes)) return plural(result.scenes.length, "scene")
    if (Array.isArray(result.assets)) return plural(result.assets.length, "file")
    if (Array.isArray(result.items) && result.applied) return "Changed " + plural(result.items.length, "item")
    if (Array.isArray(result.items)) return plural(result.items.length, "item")
    if (Array.isArray(result.words)) return plural(result.words.length, "word")
    if (typeof result.durationSeconds === "number") return result.durationSeconds.toFixed(1) + " s"
    if (typeof result.message === "string") return result.message.slice(0, 100)
    if (typeof result.summary === "string") return result.summary.slice(0, 100)
    return ""
}

// Turns a provider code into something a person can act on. connection: Retry makes sense. The provider's own message, when it
// sent one, rides along on a second line so "quota exceeded" or "model X not found" reaches the chat as the provider said it.
function friendlyError(code, message, endpoint) {
    const r = friendlyBase(code, message, endpoint)
    const said = String(message || "").replace(/\s+/g, " ").trim()
    if (said !== "" && said !== String(code) && r.text !== said) r.text += "\n" + said.slice(0, 200)
    return r
}
function friendlyBase(code, message, endpoint) {
    const where = endpoint ? " at " + endpoint : ""
    switch (code) {
    case "provider_unreachable":
        return { text: "Can’t reach the model server" + where + ". Is it running?", connection: true }
    case "provider_auth":
        return { text: "The model server rejected the API key. Check it in AI settings.", connection: false }
    case "model_not_found":
        return { text: "The model server doesn’t have that model. Pick another one in AI settings.", connection: false }
    case "rate_limited":
        return { text: "The provider is rate-limiting requests or the quota is used up. Wait a moment and retry.", connection: true }
    case "no_llm_client":
        return { text: "No model is configured. Set one in AI settings.", connection: false }
    }
    return { text: message ? message : String(code), connection: false }
}

const SPECIALIST_FOR = { color: "colourist", music: "audio", beats: "audio", audio: "audio" }
const INTENT_LABELS = {
    cut: "Cut", color: "Colour grade", music: "Music", beats: "Cut to the beat", audio: "Audio", captions: "Captions", title: "Title",
    export: "Export", assemble: "Build the cut", pauses: "Remove pauses", fillers: "Remove filler words", edit: "Edit clips"
}
function intentLabel(intent) {
    const k = String(intent || "")
    return INTENT_LABELS[k] || (k.charAt(0).toUpperCase() + k.slice(1).replace(/_/g, " "))
}
function specialistOf(step) {
    return step.specialist || SPECIALIST_FOR[step.intent] || "editor"
}

// "name(arg: value, ...)" for a proposed call in an Ask-mode confirmation
function callLine(call) {
    const label = toolLabel(call.name)
    const a = call.arguments
    if (!a || typeof a !== "object") return label
    const parts = []
    const ks = Object.keys(a)
    for (let i = 0; i < ks.length && parts.length < 3; ++i) {
        const k = ks[i]
        if (k === "session" || k === "origin") continue
        const v = a[k]
        parts.push(k + ": " + (typeof v === "object" ? JSON.stringify(v).slice(0, 40) : String(v).slice(0, 40)))
    }
    return parts.length ? label + " · " + parts.join(", ") : label
}

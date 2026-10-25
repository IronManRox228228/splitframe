import QtQuick

// Port of editor/Markdown.tsx for assistant replies: paragraphs, bold / italic / code, fenced code, lists and pipe tables, through
// Qt's CommonMark + GitHub-table renderer. The model's text is HTML-escaped first, so it can't inject markup (the Electron one built
// React elements for the same reason).
Text {
    id: md
    property string source: ""
    text: source.replace(/&/g, "&amp;").replace(/</g, "&lt;")
    textFormat: Text.MarkdownText
    color: Theme.fg2
    linkColor: Theme.accent
    font.pixelSize: 13
    lineHeight: 1.5
    lineHeightMode: Text.ProportionalHeight
    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
    onLinkActivated: link => Qt.openUrlExternally(link)
}

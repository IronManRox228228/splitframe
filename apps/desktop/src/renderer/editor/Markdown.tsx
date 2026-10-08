import { Fragment, memo, type ReactNode } from 'react';

/**
 * A deliberately small markdown renderer for assistant replies. It builds React elements
 * (never HTML strings), so model text can't inject markup. Supports paragraphs, headings
 * (as bold lines), **bold**, *italic*, `code`, fenced code, bullet/numbered lists and
 * GitHub pipe tables.
 */

const INLINE = /(`[^`\n]+`)|(\*\*[^*\n]+?\*\*)|(__[^_\n]+?__)|(\*[^*\s][^*\n]*?\*)|(\b_[^_\s][^_\n]*?_\b)/;

export function renderInline(text: string, keyPrefix = 'i'): ReactNode[] {
  const out: ReactNode[] = [];
  let rest = text;
  let n = 0;
  while (rest.length > 0) {
    const m = INLINE.exec(rest);
    if (!m) {
      out.push(rest);
      break;
    }
    if (m.index > 0) out.push(rest.slice(0, m.index));
    const tok = m[0];
    const key = `${keyPrefix}-${n++}`;
    if (tok.startsWith('`')) {
      out.push(
        <code key={key} className="px-1 py-0.5 rounded bg-white/[0.08] font-mono text-[12px] text-fg">
          {tok.slice(1, -1)}
        </code>,
      );
    } else if (tok.startsWith('**') || tok.startsWith('__')) {
      out.push(
        <strong key={key} className="font-semibold text-fg">
          {renderInline(tok.slice(2, -2), key)}
        </strong>,
      );
    } else {
      out.push(<em key={key}>{renderInline(tok.slice(1, -1), key)}</em>);
    }
    rest = rest.slice(m.index + tok.length);
  }
  return out;
}

const TABLE_SEP = /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/;
const BULLET = /^\s*[-*+]\s+(.*)$/;
const NUMBERED = /^\s*(\d+)[.)]\s+(.*)$/;

function splitRow(line: string): string[] {
  let l = line.trim();
  if (l.startsWith('|')) l = l.slice(1);
  if (l.endsWith('|')) l = l.slice(0, -1);
  return l.split('|').map((c) => c.trim());
}

type Block =
  | { t: 'p'; text: string }
  | { t: 'h'; text: string }
  | { t: 'code'; text: string }
  | { t: 'ul' | 'ol'; items: string[] }
  | { t: 'table'; head: string[]; rows: string[][] };

export function parseBlocks(src: string): Block[] {
  const lines = src.replace(/\r\n/g, '\n').split('\n');
  const blocks: Block[] = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i]!;
    if (line.trim() === '') {
      i++;
      continue;
    }
    if (/^\s*```/.test(line)) {
      const body: string[] = [];
      i++;
      while (i < lines.length && !/^\s*```/.test(lines[i]!)) body.push(lines[i++]!);
      i++; // closing fence (or end of a still-streaming block)
      blocks.push({ t: 'code', text: body.join('\n') });
      continue;
    }
    if (line.includes('|') && i + 1 < lines.length && TABLE_SEP.test(lines[i + 1]!) && lines[i + 1]!.includes('-')) {
      const head = splitRow(line);
      i += 2;
      const rows: string[][] = [];
      while (i < lines.length && lines[i]!.includes('|') && lines[i]!.trim() !== '') rows.push(splitRow(lines[i++]!));
      blocks.push({ t: 'table', head, rows });
      continue;
    }
    const h = /^#{1,6}\s+(.*)$/.exec(line);
    if (h) {
      blocks.push({ t: 'h', text: h[1]! });
      i++;
      continue;
    }
    if (BULLET.test(line)) {
      const items: string[] = [];
      while (i < lines.length && BULLET.test(lines[i]!)) items.push(BULLET.exec(lines[i++]!)![1]!);
      blocks.push({ t: 'ul', items });
      continue;
    }
    if (NUMBERED.test(line)) {
      const items: string[] = [];
      while (i < lines.length && NUMBERED.test(lines[i]!)) items.push(NUMBERED.exec(lines[i++]!)![2]!);
      blocks.push({ t: 'ol', items });
      continue;
    }
    const para: string[] = [line];
    i++;
    while (
      i < lines.length &&
      lines[i]!.trim() !== '' &&
      !/^\s*```/.test(lines[i]!) &&
      !BULLET.test(lines[i]!) &&
      !NUMBERED.test(lines[i]!) &&
      !/^#{1,6}\s/.test(lines[i]!)
    ) {
      para.push(lines[i++]!);
    }
    blocks.push({ t: 'p', text: para.join('\n') });
  }
  return blocks;
}

export const Markdown = memo(function Markdown({ text }: { text: string }) {
  const blocks = parseBlocks(text);
  return (
    <div className="flex flex-col gap-2 min-w-0">
      {blocks.map((b, idx) => {
        const key = `b${idx}`;
        switch (b.t) {
          case 'p':
            return (
              <p key={key} className="whitespace-pre-wrap break-words">
                {b.text.split('\n').map((ln, j) => (
                  <Fragment key={j}>
                    {j > 0 && <br />}
                    {renderInline(ln, `${key}-${j}`)}
                  </Fragment>
                ))}
              </p>
            );
          case 'h':
            return (
              <p key={key} className="font-semibold text-fg">
                {renderInline(b.text, key)}
              </p>
            );
          case 'code':
            return (
              <pre key={key} className="rounded-xl bg-[rgba(8,10,9,0.4)] border border-line p-2.5 font-mono text-[12px] leading-relaxed text-fg-2 overflow-x-auto">
                <code>{b.text}</code>
              </pre>
            );
          case 'ul':
            return (
              <ul key={key} className="list-disc pl-5 flex flex-col gap-1">
                {b.items.map((it, j) => (
                  <li key={j} className="break-words">
                    {renderInline(it, `${key}-${j}`)}
                  </li>
                ))}
              </ul>
            );
          case 'ol':
            return (
              <ol key={key} className="list-decimal pl-5 flex flex-col gap-1">
                {b.items.map((it, j) => (
                  <li key={j} className="break-words">
                    {renderInline(it, `${key}-${j}`)}
                  </li>
                ))}
              </ol>
            );
          case 'table':
            return (
              <div key={key} className="overflow-x-auto rounded-xl border border-line">
                <table className="text-xs border-collapse min-w-full">
                  <thead>
                    <tr className="bg-white/[0.04]">
                      {b.head.map((c, j) => (
                        <th key={j} className="text-left font-medium text-fg px-2.5 py-1.5 whitespace-nowrap">
                          {renderInline(c, `${key}-h${j}`)}
                        </th>
                      ))}
                    </tr>
                  </thead>
                  <tbody>
                    {b.rows.map((r, j) => (
                      <tr key={j} className="border-t border-line">
                        {b.head.map((_, k) => (
                          <td key={k} className="px-2.5 py-1.5 whitespace-nowrap text-fg-2">
                            {renderInline(r[k] ?? '', `${key}-${j}-${k}`)}
                          </td>
                        ))}
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            );
        }
      })}
    </div>
  );
});

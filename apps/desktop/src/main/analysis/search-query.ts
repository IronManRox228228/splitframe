/**
 * Pure helpers for transcript search. The `words` table stores ONE word per row, so a
 * multi-word query can never be satisfied by a single row: terms are OR-ed in FTS and
 * nearby hits are then grouped into phrases (see groupWordHits).
 */

export function queryTerms(query: string): string[] {
  return query
    .toLowerCase()
    .split(/\s+/)
    .map((t) => t.replaceAll('"', '').trim())
    .filter(Boolean);
}

/** FTS5 MATCH expression: every term quoted (no syntax errors on punctuation) and OR-ed. */
export function buildFtsQuery(query: string): string {
  return [...new Set(queryTerms(query))].map((t) => `"${t}"`).join(' OR ');
}

/**
 * For multi-word queries keep only phrase groups that contain every term, so searching
 * "launch date" doesn't return every place "launch" or "date" appears on its own.
 */
export function groupHasAllTerms(groupText: string, query: string): boolean {
  const terms = [...new Set(queryTerms(query))];
  if (terms.length <= 1) return true;
  const words = new Set(
    groupText
      .toLowerCase()
      .split(/[^\p{L}\p{N}']+/u)
      .filter(Boolean),
  );
  return terms.every((t) => words.has(t) || t.split(/[^\p{L}\p{N}']+/u).filter(Boolean).every((p) => words.has(p)));
}

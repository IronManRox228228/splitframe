import type { Op, TimelineDoc } from '@cutboard/schema';

/** The slice of better-sqlite3 the project store needs (lets tests run against a fake). */
export interface SqlDb {
  prepare(sql: string): { run(...args: unknown[]): { lastInsertRowid: number | bigint } };
  transaction<T>(fn: () => T): () => T;
}

/**
 * Persist one timeline change: the op-log row and the doc snapshot (plus the project name
 * used by the recents list) are written in ONE transaction, so a crash can never leave the
 * log ahead of the snapshot or the other way round. Returns the new op_log sequence number
 * (0 when nothing was logged).
 */
export function commitProjectDoc(
  db: SqlDb,
  change: { projectId: string; doc: TimelineDoc; log?: { actor: string; ops: Op[] }; now: string },
): number {
  const { projectId, doc, log, now } = change;
  const write = db.transaction(() => {
    let seq = 0;
    if (log && log.ops.length > 0) {
      const payload = log.ops.length === 1 ? log.ops[0] : { type: 'batch', ops: log.ops };
      const info = db
        .prepare(`INSERT INTO op_log (project_id, actor, op, created_at) VALUES (?, ?, ?, ?)`)
        .run(projectId, log.actor, JSON.stringify(payload), now);
      seq = Number(info.lastInsertRowid);
    }
    // the name lives in the doc (project.rename is an op) and in the column the recents list reads
    db.prepare(`UPDATE projects SET name=?, doc=?, updated_at=? WHERE id=?`).run(doc.project.name, JSON.stringify(doc), now, projectId);
    return seq;
  });
  return write();
}

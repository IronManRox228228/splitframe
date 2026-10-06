import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { createEmptyDoc } from '@cutboard/editor-core';
import { commitProjectDoc, type SqlDb } from './project-store.ts';

/** Records statements; a transaction rolls back everything written inside it when it throws. */
function fakeDb(opts: { failOn?: string } = {}) {
  const committed: { sql: string; args: unknown[] }[] = [];
  let pending: { sql: string; args: unknown[] }[] | null = null;
  const db: SqlDb = {
    prepare(sql) {
      return {
        run(...args) {
          if (opts.failOn && sql.includes(opts.failOn)) throw new Error('disk full');
          (pending ?? committed).push({ sql, args });
          return { lastInsertRowid: 41 + committed.length + (pending?.length ?? 0) };
        },
      };
    },
    transaction(fn) {
      return () => {
        pending = [];
        try {
          const out = fn();
          committed.push(...pending);
          return out;
        } finally {
          pending = null;
        }
      };
    },
  };
  return { db, committed };
}

const doc = (name: string) => createEmptyDoc({ id: newId('prj'), name, fps: 30 });

describe('commitProjectDoc', () => {
  it('writes the log row and the snapshot together and returns the sequence number', () => {
    const { db, committed } = fakeDb();
    const d = doc('Renamed');
    const seq = commitProjectDoc(db, {
      projectId: d.project.id,
      doc: d,
      log: { actor: 'user', ops: [{ type: 'project.rename', name: 'Renamed' }] },
      now: '2026-01-01T00:00:00.000Z',
    });
    expect(seq).toBeGreaterThan(0);
    expect(committed.map((c) => c.sql.split(' ')[0])).toEqual(['INSERT', 'UPDATE']);
    expect(JSON.parse(committed[0]!.args[2] as string)).toEqual({ type: 'project.rename', name: 'Renamed' });
  });

  it('keeps the recents-list name in sync with project.rename', () => {
    const { db, committed } = fakeDb();
    const d = doc('New name');
    commitProjectDoc(db, { projectId: d.project.id, doc: d, now: 'now' });
    const update = committed.find((c) => c.sql.startsWith('UPDATE projects'))!;
    expect(update.args[0]).toBe('New name');
  });

  it('batches several ops into one log entry', () => {
    const { db, committed } = fakeDb();
    const d = doc('x');
    commitProjectDoc(db, {
      projectId: d.project.id,
      doc: d,
      log: { actor: 'builtin-agent', ops: [{ type: 'project.rename', name: 'a' }, { type: 'project.rename', name: 'b' }] },
      now: 'now',
    });
    expect(JSON.parse(committed[0]!.args[2] as string).type).toBe('batch');
  });

  it('does not leave a log row behind when the snapshot write fails', () => {
    const { db, committed } = fakeDb({ failOn: 'UPDATE projects' });
    const d = doc('x');
    expect(() =>
      commitProjectDoc(db, { projectId: d.project.id, doc: d, log: { actor: 'user', ops: [{ type: 'project.rename', name: 'x' }] }, now: 'now' }),
    ).toThrow('disk full');
    expect(committed).toEqual([]);
  });

  it('skips the log for an empty op list (nothing to record)', () => {
    const { db, committed } = fakeDb();
    const d = doc('x');
    expect(commitProjectDoc(db, { projectId: d.project.id, doc: d, log: { actor: 'user', ops: [] }, now: 'now' })).toBe(0);
    expect(committed).toHaveLength(1);
  });
});

import { getDb } from './db.ts';
import { newId } from '@cutboard/schema';

export interface JobRow {
  id: string;
  projectId: string | null;
  type: string;
  payload: Record<string, unknown>;
  status: 'pending' | 'running' | 'done' | 'failed' | 'cancelled';
  progress: number;
  stage: string | null;
  error: string | null;
  createdAt: string;
  updatedAt: string;
}

type Listener = (job: JobRow) => void;
const listeners = new Set<Listener>();

export function onJobEvent(listener: Listener): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function emit(job: JobRow): void {
  for (const l of listeners) l(job);
}

function rowFromDb(r: Record<string, unknown>): JobRow {
  return {
    id: r.id as string,
    projectId: (r.project_id as string) ?? null,
    type: r.type as string,
    payload: JSON.parse((r.payload as string) ?? '{}'),
    status: r.status as JobRow['status'],
    progress: r.progress as number,
    stage: (r.stage as string) ?? null,
    error: (r.error as string) ?? null,
    createdAt: r.created_at as string,
    updatedAt: r.updated_at as string,
  };
}

/**
 * SQLite-backed job queue (addendum §2). Sequential per app instance; jobs are
 * resumable — on boot anything still 'running' is reset to 'pending'.
 */
class JobManager {
  private queue: string[] = [];
  private active: JobRow | null = null;
  private cancelFlags = new Map<string, AbortController>();
  private handlers = new Map<
    string,
    (job: JobRow, ctx: { signal: AbortSignal; progress: (p: number, stage?: string) => void }) => Promise<void>
  >();

  register(
    type: string,
    handler: (job: JobRow, ctx: { signal: AbortSignal; progress: (p: number, stage?: string) => void }) => Promise<void>,
  ): void {
    this.handlers.set(type, handler);
  }

  resumePending(): void {
    const db = getDb();
    db.prepare(`UPDATE jobs SET status='pending', updated_at=? WHERE status IN ('running')`).run(
      new Date().toISOString(),
    );
    const pending = db
      .prepare(`SELECT id FROM jobs WHERE status='pending' ORDER BY created_at`)
      .all() as { id: string }[];
    this.queue.push(...pending.map((p) => p.id));
    this.pump();
  }

  enqueue(type: string, projectId: string | null, payload: Record<string, unknown>): JobRow {
    const db = getDb();
    const job: JobRow = {
      id: newId('job'),
      projectId,
      type,
      payload,
      status: 'pending',
      progress: 0,
      stage: null,
      error: null,
      createdAt: new Date().toISOString(),
      updatedAt: new Date().toISOString(),
    };
    db.prepare(
      `INSERT INTO jobs (id, project_id, type, payload, status, progress, created_at, updated_at)
       VALUES (?, ?, ?, ?, 'pending', 0, ?, ?)`,
    ).run(job.id, job.projectId, job.type, JSON.stringify(payload), job.createdAt, job.updatedAt);
    this.queue.push(job.id);
    this.pump();
    emit(job);
    return job;
  }

  cancel(jobId: string): void {
    this.cancelFlags.get(jobId)?.abort();
    if (this.active?.id !== jobId) {
      const db = getDb();
      db.prepare(`UPDATE jobs SET status='cancelled', updated_at=? WHERE id=? AND status='pending'`).run(
        new Date().toISOString(),
        jobId,
      );
      this.queue = this.queue.filter((id) => id !== jobId);
      emit(this.get(jobId)!);
    }
  }

  get(jobId: string): JobRow | null {
    const db = getDb();
    const row = db.prepare(`SELECT * FROM jobs WHERE id=?`).get(jobId) as Record<string, unknown> | undefined;
    return row ? rowFromDb(row) : null;
  }

  list(projectId?: string): JobRow[] {
    const db = getDb();
    const rows = (
      projectId
        ? db.prepare(`SELECT * FROM jobs WHERE project_id=? ORDER BY created_at DESC LIMIT 200`).all(projectId)
        : db.prepare(`SELECT * FROM jobs ORDER BY created_at DESC LIMIT 200`).all()
    ) as Record<string, unknown>[];
    return rows.map(rowFromDb);
  }

  private pump(): void {
    if (this.active || this.queue.length === 0) return;
    const nextId = this.queue.shift()!;
    void this.run(nextId);
  }

  private async run(jobId: string): Promise<void> {
    const db = getDb();
    const job = this.get(jobId);
    if (!job || job.status !== 'pending') {
      this.pump();
      return;
    }
    const handler = this.handlers.get(job.type);
    const controller = new AbortController();
    this.cancelFlags.set(jobId, controller);
    this.active = job;
    this.update(jobId, { status: 'running' });
    try {
      if (!handler) throw new Error(`No handler registered for job type ${job.type}`);
      await handler(job, {
        signal: controller.signal,
        progress: (p, stage) => this.update(jobId, { progress: Math.min(1, Math.max(0, p)), stage }),
      });
      this.update(jobId, { status: 'done', progress: 1 });
    } catch (err) {
      const aborted = controller.signal.aborted;
      this.update(jobId, {
        status: aborted ? 'cancelled' : 'failed',
        error: aborted ? null : err instanceof Error ? err.message : String(err),
      });
    } finally {
      this.cancelFlags.delete(jobId);
      this.active = null;
      this.pump();
    }
  }

  private update(jobId: string, patch: Partial<Pick<JobRow, 'status' | 'progress' | 'stage' | 'error'>>): void {
    const db = getDb();
    const sets: string[] = ['updated_at=?'];
    const vals: unknown[] = [new Date().toISOString()];
    if (patch.status !== undefined) {
      sets.push('status=?');
      vals.push(patch.status);
    }
    if (patch.progress !== undefined) {
      sets.push('progress=?');
      vals.push(patch.progress);
    }
    if (patch.stage !== undefined) {
      sets.push('stage=?');
      vals.push(patch.stage);
    }
    if (patch.error !== undefined) {
      sets.push('error=?');
      vals.push(patch.error);
    }
    vals.push(jobId);
    db.prepare(`UPDATE jobs SET ${sets.join(', ')} WHERE id=?`).run(...vals);
    const updated = this.get(jobId);
    if (updated) emit(updated);
  }
}

export const jobs = new JobManager();

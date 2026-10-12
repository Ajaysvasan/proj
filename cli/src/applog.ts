import { appendFileSync, mkdirSync, readFileSync, renameSync, statSync } from 'node:fs';
import { join } from 'node:path';

// Application log shared with the engine: <data_dir>/logs/proj.log, JSON lines,
// rotated at ~1 MiB with three backups so it never grows without bound.
const MAX_BYTES = 1024 * 1024;
const KEEP = 3;

export type LogLevel = 'DEBUG' | 'INFO' | 'WARN' | 'ERROR';

export interface AppLogEntry {
  ts: string;
  level: LogLevel;
  source: string;
  component?: string;
  message: string;
  pid?: number;
  [key: string]: unknown;
}

export function logFile(dataDir: string): string {
  return join(dataDir, 'logs', 'proj.log');
}

function rotate(file: string): void {
  try {
    if (statSync(file).size < MAX_BYTES) return;
  } catch {
    return;
  }
  for (let i = KEEP - 1; i >= 1; i--) {
    try {
      renameSync(`${file}.${i}`, `${file}.${i + 1}`);
    } catch {
      // missing backup: nothing to shift
    }
  }
  try {
    renameSync(file, `${file}.1`);
  } catch {
    // another process rotated it first
  }
}

/** Appends one entry. Logging failures are reported on stderr, never thrown. */
export function appendAppLog(dataDir: string, level: LogLevel, component: string, message: string, extra: Record<string, unknown> = {}): void {
  const file = logFile(dataDir);
  try {
    mkdirSync(join(dataDir, 'logs'), { recursive: true });
    rotate(file);
    const entry: AppLogEntry = { ts: new Date().toISOString(), level, source: 'cli', pid: process.pid, component, message, ...extra };
    appendFileSync(file, JSON.stringify(entry) + '\n');
  } catch (err) {
    process.stderr.write(`proj: could not write application log ${file}: ${(err as Error).message}\n`);
  }
}

/** Reads the most recent entries (newest first) at or above `minLevel`. */
export function readAppLog(dataDir: string, limit: number, minLevel: LogLevel = 'WARN'): AppLogEntry[] {
  const order: Record<LogLevel, number> = { DEBUG: 0, INFO: 1, WARN: 2, ERROR: 3 };
  const file = logFile(dataDir);
  const entries: AppLogEntry[] = [];
  for (const path of [file, `${file}.1`]) {
    let text: string;
    try {
      text = readFileSync(path, 'utf8');
    } catch {
      continue;
    }
    const lines = text.split('\n').filter((l) => l.trim().length > 0).reverse();
    for (const line of lines) {
      try {
        const e = JSON.parse(line) as AppLogEntry;
        if (order[e.level] !== undefined && order[e.level] >= order[minLevel]) entries.push(e);
      } catch {
        // a torn or foreign line is skipped
      }
      if (entries.length >= limit) return entries;
    }
  }
  return entries;
}

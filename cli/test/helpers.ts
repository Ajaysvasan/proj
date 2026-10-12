import { type ChildProcess, spawn, spawnSync } from 'node:child_process';
import { chmodSync, cpSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { locateEngine } from '../src/engine/locate.js';

const here = dirname(fileURLToPath(import.meta.url));
/** cli/dist/test -> repository root */
export const REPO = resolve(here, '..', '..', '..');
export const CLI = join(REPO, 'cli', 'dist', 'src', 'index.js');
export const FIXTURES = join(REPO, 'fixtures');
export const ENGINE = locateEngine();

const created: string[] = [];

/** A private temporary directory; removed by cleanupTemp(). */
export function tempDir(label = 'tmp'): string {
  const dir = mkdtempSync(join(tmpdir(), `proj-cli-test-${label}-`));
  created.push(dir);
  return dir;
}

export function cleanupTemp(): void {
  for (const dir of created.splice(0)) {
    // Only directories this suite created under the system temp directory.
    if (basename(dir).startsWith('proj-cli-test-')) rmSync(dir, { recursive: true, force: true });
  }
}

/** Copies a fixture into a temp dir (optionally under a name with spaces). */
export function copyFixture(name: string, target: string): string {
  const dest = join(target, name);
  cpSync(join(FIXTURES, name), dest, { recursive: true });
  return dest;
}

/** Writes an executable POSIX shell script used as a fake engine. */
export function fakeEngine(dir: string, name: string, body: string): string {
  const path = join(dir, name);
  writeFileSync(path, `#!/bin/sh\n${body}\n`);
  chmodSync(path, 0o755);
  return path;
}

export interface CliResult {
  code: number | null;
  stdout: string;
  stderr: string;
}

export function runCli(args: string[], env: Record<string, string> = {}): CliResult {
  const r = spawnSync(process.execPath, [CLI, ...args], {
    encoding: 'utf8',
    env: { ...process.env, NO_COLOR: '1', FORCE_COLOR: '0', PROJ_ENGINE: ENGINE, ...env },
    timeout: 120000,
  });
  return { code: r.status, stdout: r.stdout, stderr: r.stderr };
}

/** Starts the CLI without waiting for it (for tests that interact with its pipes). */
export function spawnCli(args: string[], env: Record<string, string> = {}): ChildProcess {
  return spawn(process.execPath, [CLI, ...args], {
    env: { ...process.env, NO_COLOR: '1', FORCE_COLOR: '0', PROJ_ENGINE: ENGINE, ...env },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}

import { accessSync, constants, statSync } from 'node:fs';
import { delimiter, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export class EngineNotFoundError extends Error {}

function isExecutable(path: string): boolean {
  try {
    if (!statSync(path).isFile()) return false;
    accessSync(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * Finds the proj-engine binary without depending on the working directory:
 *   1. explicit path (--engine option)
 *   2. $PROJ_ENGINE
 *   3. <repo>/build/engine/proj-engine, located relative to this file
 *   4. proj-engine on $PATH
 */
export function locateEngine(explicit?: string, env: NodeJS.ProcessEnv = process.env): string {
  const candidates: { path: string; source: string }[] = [];
  if (explicit) candidates.push({ path: resolve(explicit), source: '--engine' });
  if (env.PROJ_ENGINE) candidates.push({ path: resolve(env.PROJ_ENGINE), source: 'PROJ_ENGINE' });
  if (explicit || env.PROJ_ENGINE) {
    const c = candidates[0]!;
    if (isExecutable(c.path)) return c.path;
    throw new EngineNotFoundError(`proj-engine not found or not executable at ${c.path} (from ${c.source})`);
  }
  // dist/src/engine/locate.js -> repository root is four levels up.
  const here = dirname(fileURLToPath(import.meta.url));
  const repo = resolve(here, '..', '..', '..', '..');
  const built = join(repo, 'build', 'engine', 'proj-engine');
  if (isExecutable(built)) return built;
  for (const dir of (env.PATH ?? '').split(delimiter)) {
    if (!dir) continue;
    const p = join(dir, 'proj-engine');
    if (isExecutable(p)) return p;
  }
  throw new EngineNotFoundError(
    `proj-engine was not found. Build it with "cmake -S engine -B build/engine && cmake --build build/engine" ` +
      `or set PROJ_ENGINE to its path (looked in ${built} and on PATH).`,
  );
}

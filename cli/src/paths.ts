import { homedir, platform } from 'node:os';
import { isAbsolute, join, resolve } from 'node:path';

/**
 * Data directory holding registry.db, indexes/ and logs/. The CLI resolves it
 * once and passes it to the engine explicitly, so both always agree.
 *   1. --data-dir option
 *   2. $PROJ_HOME
 *   3. macOS: ~/Library/Application Support/proj
 *      other: $XDG_DATA_HOME/proj or ~/.local/share/proj
 */
export function resolveDataDir(option?: string, env: NodeJS.ProcessEnv = process.env, os = platform()): string {
  if (option && option.length > 0) return resolve(option);
  if (env.PROJ_HOME && env.PROJ_HOME.length > 0) return resolve(env.PROJ_HOME);
  const home = env.HOME && env.HOME.length > 0 ? env.HOME : homedir();
  if (os === 'darwin') return join(home, 'Library', 'Application Support', 'proj');
  if (env.XDG_DATA_HOME && isAbsolute(env.XDG_DATA_HOME)) return join(env.XDG_DATA_HOME, 'proj');
  return join(home, '.local', 'share', 'proj');
}

// Client side of the dynamic tracing service (`proj-engine --dynamic-service`): finds or starts
// it, and sends it one request per connection over 127.0.0.1 with the token from its service file.
import { spawn } from 'node:child_process';
import { accessSync, constants, readFileSync } from 'node:fs';
import { connect } from 'node:net';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CliError, type Context } from '../context.js';
import { ExitCode } from '../exit-codes.js';

export interface ServiceInfo {
  pid: number;
  port: number;
  token: string;
  version: string;
  started_at: string;
  agent_jar: string;
}

export function serviceDir(dataDir: string): string {
  return join(dataDir, 'dynamic');
}

export function readServiceInfo(dataDir: string): ServiceInfo | null {
  try {
    const info = JSON.parse(readFileSync(join(serviceDir(dataDir), 'service.json'), 'utf8'));
    return typeof info.port === 'number' && typeof info.token === 'string' ? info : null;
  } catch {
    return null;
  }
}

/** Projects the service was monitoring when it last ran (resumed when it starts again). */
export function savedMonitors(dataDir: string): { project: string; pid?: number; match?: string }[] {
  try {
    const saved = JSON.parse(readFileSync(join(serviceDir(dataDir), 'monitors.json'), 'utf8'));
    return Array.isArray(saved.monitors) ? saved.monitors : [];
  } catch {
    return [];
  }
}

function readable(path: string): boolean {
  try {
    accessSync(path, constants.R_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * proj-agent.jar: $PROJ_AGENT_JAR, next to the engine (bin/), the engine build tree
 * (build/engine/agent/), or this checkout's build tree.
 */
export function locateAgentJar(enginePath: string, env: NodeJS.ProcessEnv = process.env): string | null {
  const here = dirname(fileURLToPath(import.meta.url));
  const candidates = [
    env.PROJ_AGENT_JAR,
    join(dirname(enginePath), 'proj-agent.jar'),
    join(dirname(enginePath), 'agent', 'proj-agent.jar'),
    resolve(here, '..', '..', '..', '..', 'build', 'engine', 'agent', 'proj-agent.jar'),
  ];
  for (const c of candidates) {
    if (c && readable(c)) return resolve(c);
  }
  return null;
}

/** JDK used to attach to running JVMs: $PROJ_JAVA, $JAVA_HOME/bin/java, or java on PATH. */
export function javaCommand(env: NodeJS.ProcessEnv = process.env): string {
  if (env.PROJ_JAVA) return env.PROJ_JAVA;
  if (env.JAVA_HOME) return join(env.JAVA_HOME, 'bin', 'java');
  return 'java';
}

export function request(info: ServiceInfo, req: Record<string, unknown>, timeoutMs = 10000): Promise<any> {
  return new Promise((resolvePromise, reject) => {
    const sock = connect({ host: '127.0.0.1', port: info.port });
    let buf = '';
    let done = false;
    const finish = (err?: Error, value?: unknown) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      sock.destroy();
      if (err) reject(err);
      else resolvePromise(value);
    };
    const timer = setTimeout(() => finish(new Error(`the dynamic service did not answer within ${timeoutMs / 1000} s`)), timeoutMs);
    sock.on('connect', () => sock.write(`${JSON.stringify({ ...req, token: info.token })}\n`));
    sock.on('data', (d) => {
      buf += d.toString('utf8');
      const nl = buf.indexOf('\n');
      if (nl < 0) return;
      try {
        finish(undefined, JSON.parse(buf.slice(0, nl)));
      } catch (e) {
        finish(e as Error);
      }
    });
    sock.on('error', (e) => finish(e));
    sock.on('close', () => finish(new Error('the dynamic service closed the connection')));
  });
}

/** The running service, or null (no service file, or nobody answers on its port). */
export async function runningService(dataDir: string): Promise<ServiceInfo | null> {
  const info = readServiceInfo(dataDir);
  if (!info) return null;
  try {
    await request(info, { op: 'status' }, 3000);
    return info;
  } catch {
    return null;
  }
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** The running service, started first if needed (a detached process that outlives this command). */
export async function ensureService(ctx: Context): Promise<ServiceInfo> {
  const existing = await runningService(ctx.dataDir);
  if (existing) return existing;
  const jar = locateAgentJar(ctx.enginePath);
  if (!jar) {
    throw new CliError(
      'the Java agent (proj-agent.jar) was not found; build it with a JDK installed (scripts/build.sh) or set PROJ_AGENT_JAR',
      ExitCode.InternalError,
    );
  }
  const before = readServiceInfo(ctx.dataDir)?.started_at;
  const child = spawn(ctx.enginePath, ['--dynamic-service', '--data-dir', ctx.dataDir, '--agent-jar', jar, '--java', javaCommand()], {
    detached: true,
    stdio: 'ignore',
  });
  child.unref();
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    await sleep(50);
    const info = readServiceInfo(ctx.dataDir);
    if (!info || info.started_at === before) continue;
    try {
      await request(info, { op: 'status' }, 2000);
      return info;
    } catch {
      // not listening yet
    }
  }
  throw new CliError(`the dynamic service did not start; see ${join(ctx.dataDir, 'logs', 'dynamic.log')}`, ExitCode.InternalError);
}

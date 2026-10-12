import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { createInterface } from 'node:readline';

import { PROTOCOL_VERSION, validateEnvelope, type EngineRequest, type EngineResponse } from './protocol.js';

/** Failure of the engine process itself (as opposed to an error response). */
export class EngineProcessError extends Error {
  constructor(
    readonly kind: 'spawn_failed' | 'crashed' | 'timeout' | 'malformed_response' | 'closed',
    message: string,
    readonly stderr = '',
    readonly exitCode: number | null = null,
    readonly signal: NodeJS.Signals | null = null,
  ) {
    super(message);
  }
}

export interface EngineOptions {
  enginePath: string;
  dataDir: string;
  logLevel?: 'debug' | 'info' | 'warn' | 'error';
  timeoutMs?: number;
  /** Receives engine stderr as it arrives (verbose mode). */
  onStderr?: (chunk: string) => void;
}

const MAX_STDOUT_BYTES = 64 * 1024 * 1024;
const STDERR_TAIL_BYTES = 16 * 1024;

let nextId = 1;

function buildRequest(op: string, args: Record<string, unknown>, opts: EngineOptions): EngineRequest {
  return {
    protocol: PROTOCOL_VERSION,
    id: nextId++,
    op,
    args,
    options: { data_dir: opts.dataDir, log_level: opts.logLevel ?? 'warn' },
  };
}

function decode(text: string, stderr: string): EngineResponse {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    throw new EngineProcessError('malformed_response', 'engine returned output that is not valid JSON', stderr);
  }
  const problem = validateEnvelope(value);
  if (problem) throw new EngineProcessError('malformed_response', `engine returned an invalid response: ${problem}`, stderr);
  return value as EngineResponse;
}

function tail(s: string): string {
  return s.length > STDERR_TAIL_BYTES ? s.slice(s.length - STDERR_TAIL_BYTES) : s;
}

/**
 * Runs one request in a fresh engine process. Arguments are passed as a JSON
 * document on stdin, never through a shell, so paths and queries cannot be
 * interpreted as commands.
 */
export function runEngine<T = any>(op: string, args: Record<string, unknown>, opts: EngineOptions): Promise<EngineResponse<T>> {
  const request = buildRequest(op, args, opts);
  return new Promise((resolvePromise, reject) => {
    let child: ChildProcessWithoutNullStreams;
    try {
      child = spawn(opts.enginePath, [], { stdio: ['pipe', 'pipe', 'pipe'], shell: false });
    } catch (err) {
      reject(new EngineProcessError('spawn_failed', `could not start proj-engine: ${(err as Error).message}`));
      return;
    }
    let stdout = '';
    let stderr = '';
    let settled = false;
    let timedOut = false;
    const finish = (fn: () => void) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      fn();
    };
    const timeoutMs = opts.timeoutMs ?? 0;
    const timer: NodeJS.Timeout | undefined =
      timeoutMs > 0
        ? setTimeout(() => {
            timedOut = true;
            child.kill('SIGTERM');
            setTimeout(() => child.kill('SIGKILL'), 2000).unref();
          }, timeoutMs)
        : undefined;

    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk: string) => {
      stdout += chunk;
      if (stdout.length > MAX_STDOUT_BYTES) {
        child.kill('SIGKILL');
        finish(() => reject(new EngineProcessError('malformed_response', 'engine output exceeded 64 MiB', tail(stderr))));
      }
    });
    child.stderr.on('data', (chunk: string) => {
      stderr = tail(stderr + chunk);
      opts.onStderr?.(chunk);
    });
    child.on('error', (err) => {
      finish(() => reject(new EngineProcessError('spawn_failed', `could not start proj-engine: ${err.message}`, tail(stderr))));
    });
    child.on('close', (code, signal) => {
      finish(() => {
        if (timedOut) {
          reject(new EngineProcessError('timeout', `engine did not respond within ${timeoutMs} ms`, tail(stderr), code, signal));
          return;
        }
        const line = stdout.trim().split('\n').pop() ?? '';
        if (code !== 0 || signal) {
          // A fatal path may still emit an internal_error envelope; prefer it when valid.
          try {
            const res = decode(line, stderr);
            if (res.status === 'internal_error') {
              resolvePromise(res as EngineResponse<T>);
              return;
            }
          } catch {
            // fall through to the crash report
          }
          const how = signal ? `was killed by ${signal}` : `exited with code ${code}`;
          reject(new EngineProcessError('crashed', `proj-engine ${how}`, tail(stderr), code, signal));
          return;
        }
        if (line.length === 0) {
          reject(new EngineProcessError('malformed_response', 'engine exited without a response', tail(stderr), code));
          return;
        }
        try {
          resolvePromise(decode(line, stderr) as EngineResponse<T>);
        } catch (err) {
          reject(err);
        }
      });
    });
    child.stdin.on('error', () => {
      // EPIPE when the engine exits early; reported through 'close'.
    });
    child.stdin.end(JSON.stringify(request));
  });
}

interface Pending {
  resolve: (r: EngineResponse) => void;
  reject: (e: Error) => void;
  timer: NodeJS.Timeout;
  /** The process the request was written to; only its failure rejects the request. */
  owner: ChildProcessWithoutNullStreams;
}

/**
 * A long-lived `proj-engine --serve` process used by the interactive UI.
 * Requests are newline-delimited JSON; responses are matched by id. The
 * process is restarted transparently after a crash or timeout.
 */
export class EngineSession {
  private child: ChildProcessWithoutNullStreams | null = null;
  private pending = new Map<number, Pending>();
  private stderr = '';
  private closed = false;

  constructor(private readonly opts: EngineOptions) {}

  private start(): ChildProcessWithoutNullStreams {
    const child = spawn(this.opts.enginePath, ['--serve'], { stdio: ['pipe', 'pipe', 'pipe'], shell: false });
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk: string) => {
      this.stderr = tail(this.stderr + chunk);
      this.opts.onStderr?.(chunk);
    });
    const lines = createInterface({ input: child.stdout });
    lines.on('line', (line) => this.onLine(child, line));
    child.on('error', (err) => this.failAll(child, new EngineProcessError('spawn_failed', `could not start proj-engine: ${err.message}`)));
    child.on('close', (code, signal) => {
      if (this.child === child) this.child = null;
      const how = signal ? `was killed by ${signal}` : `exited with code ${code}`;
      this.failAll(child, new EngineProcessError(this.closed ? 'closed' : 'crashed', `proj-engine ${how}`, this.stderr, code, signal));
    });
    child.stdin.on('error', () => {
      // reported through 'close'
    });
    return child;
  }

  private onLine(child: ChildProcessWithoutNullStreams, line: string): void {
    if (line.trim().length === 0) return;
    let res: EngineResponse;
    try {
      res = decode(line, this.stderr);
    } catch (err) {
      // Without a valid id the response cannot be matched: fail everything sent to this process.
      this.failAll(child, err as Error);
      if (this.child === child) this.restart();
      return;
    }
    const id = typeof res.id === 'number' ? res.id : -1;
    const p = this.pending.get(id);
    if (!p) return;
    this.pending.delete(id);
    clearTimeout(p.timer);
    p.resolve(res);
  }

  private failAll(owner: ChildProcessWithoutNullStreams, err: Error): void {
    for (const [id, p] of this.pending) {
      if (p.owner !== owner) continue;
      clearTimeout(p.timer);
      this.pending.delete(id);
      p.reject(err);
    }
  }

  private restart(): void {
    const c = this.child;
    this.child = null;
    c?.kill('SIGTERM');
  }

  request<T = any>(op: string, args: Record<string, unknown>): Promise<EngineResponse<T>> {
    if (this.closed) return Promise.reject(new EngineProcessError('closed', 'engine session is closed'));
    if (!this.child) this.child = this.start();
    const child = this.child;
    const req = buildRequest(op, args, this.opts);
    const timeoutMs = this.opts.timeoutMs ?? 30000;
    return new Promise((resolvePromise, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(req.id);
        reject(new EngineProcessError('timeout', `engine did not answer "${op}" within ${timeoutMs} ms`, this.stderr));
        if (this.child === child) this.restart();
      }, timeoutMs);
      this.pending.set(req.id, { resolve: resolvePromise as (r: EngineResponse) => void, reject, timer, owner: child });
      child.stdin.write(JSON.stringify(req) + '\n');
    });
  }

  /** Ends the session: closes stdin so the engine exits on EOF. */
  close(): Promise<void> {
    this.closed = true;
    const child = this.child;
    this.child = null;
    if (!child) return Promise.resolve();
    return new Promise((resolveClose) => {
      const force = setTimeout(() => child.kill('SIGKILL'), 2000);
      force.unref();
      child.once('close', () => {
        clearTimeout(force);
        resolveClose();
      });
      child.stdin.end();
    });
  }

  get running(): boolean {
    return this.child !== null;
  }
}

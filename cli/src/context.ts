import chalk from 'chalk';

import { appendAppLog } from './applog.js';
import { EngineProcessError, type EngineOptions } from './engine/client.js';
import { locateEngine } from './engine/locate.js';
import type { EngineResponse, EngineStatus } from './engine/protocol.js';
import { ExitCode } from './exit-codes.js';
import { resolveDataDir } from './paths.js';

export interface GlobalOptions {
  dataDir?: string;
  engine?: string;
  verbose?: boolean;
  debug?: boolean;
  quiet?: boolean;
}

export interface Context {
  dataDir: string;
  enginePath: string;
  verbose: boolean;
  quiet: boolean;
  logLevel: 'debug' | 'info' | 'warn' | 'error';
}

/** An error already explained to the user; carries the exit code. */
export class CliError extends Error {
  constructor(message: string, readonly exitCode: number) {
    super(message);
  }
}

export function makeContext(opts: GlobalOptions): Context {
  const dataDir = resolveDataDir(opts.dataDir);
  let enginePath: string;
  try {
    enginePath = locateEngine(opts.engine);
  } catch (err) {
    appendAppLog(dataDir, 'ERROR', 'cli', (err as Error).message, { kind: 'engine_not_found' });
    throw new CliError((err as Error).message, ExitCode.InternalError);
  }
  return {
    dataDir,
    enginePath,
    verbose: Boolean(opts.verbose || opts.debug),
    quiet: Boolean(opts.quiet),
    logLevel: opts.debug ? 'debug' : opts.verbose ? 'info' : 'warn',
  };
}

export function engineOptions(ctx: Context, extra: Partial<EngineOptions> = {}): EngineOptions {
  return {
    enginePath: ctx.enginePath,
    dataDir: ctx.dataDir,
    logLevel: ctx.logLevel,
    onStderr: ctx.verbose ? (chunk) => process.stderr.write(chunk) : undefined,
    ...extra,
  };
}

/** Exit code for a non-success engine status. */
export function exitCodeForStatus(status: EngineStatus): number {
  switch (status) {
    case 'ok':
      return ExitCode.Success;
    case 'partial':
      return ExitCode.Partial;
    case 'invalid_arguments':
    case 'not_found':
    case 'conflict':
      return ExitCode.UserError;
    default:
      return ExitCode.InternalError;
  }
}

const USER_STATUSES: EngineStatus[] = ['invalid_arguments', 'not_found', 'conflict'];

/**
 * Converts an error response into a CliError. User errors are explained
 * briefly; infrastructure errors are also written to the application log.
 */
export function responseError(ctx: Context, op: string, res: EngineResponse): CliError {
  const message = res.error?.message ?? `engine reported ${res.status}`;
  if (!USER_STATUSES.includes(res.status)) {
    appendAppLog(ctx.dataDir, 'ERROR', 'engine', message, { op, status: res.status, code: res.error?.code });
  }
  return new CliError(message, exitCodeForStatus(res.status));
}

/** Converts a process-level engine failure (crash, timeout, bad output) into a CliError. */
export function processError(ctx: Context, op: string, err: unknown): CliError {
  if (err instanceof CliError) return err;
  if (err instanceof EngineProcessError) {
    appendAppLog(ctx.dataDir, 'ERROR', 'engine-client', err.message, {
      op,
      kind: err.kind,
      exit_code: err.exitCode,
      signal: err.signal,
      stderr_tail: err.stderr.slice(-2000),
    });
    let message = `${err.message} while running "${op}"`;
    if (err.stderr.trim().length > 0) message += `\n${chalk.dim(err.stderr.trim().split('\n').slice(-5).join('\n'))}`;
    return new CliError(message, ExitCode.InternalError);
  }
  const message = err instanceof Error ? err.message : String(err);
  appendAppLog(ctx.dataDir, 'ERROR', 'cli', message, { op });
  return new CliError(message, ExitCode.InternalError);
}

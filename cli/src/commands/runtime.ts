// proj errors|traces <project> [id]: what runtime tracing recorded (works without the service).
import chalk from 'chalk';

import { CliError, type Context, engineOptions, processError, responseError } from '../context.js';
import { runEngine } from '../engine/client.js';
import { ExitCode } from '../exit-codes.js';
import { durationText, errorReport, shortClass, STATE_TEXT, traceLabel, traceLines } from '../ui/runtime_text.js';

export interface RuntimeOptions {
  json?: boolean;
  limit?: string;
}

async function call(ctx: Context, op: string, args: Record<string, unknown>) {
  let res;
  try {
    res = await runEngine(op, args, engineOptions(ctx));
  } catch (err) {
    throw processError(ctx, op, err);
  }
  if (res.status !== 'ok') throw responseError(ctx, op, res);
  return res.result;
}

function parseId(value: string | undefined, what: string): number | undefined {
  if (value === undefined) return undefined;
  const n = Number(value);
  if (!Number.isInteger(n) || n < 1) throw new CliError(`invalid ${what} id "${value}"`, ExitCode.UserError);
  return n;
}

function parseLimit(value: string | undefined): number {
  if (value === undefined) return 50;
  const n = Number(value);
  if (!Number.isInteger(n) || n < 1) throw new CliError(`invalid --limit "${value}"`, ExitCode.UserError);
  return n;
}

const NOTHING = 'Nothing has been observed for this project yet: run `proj start dynamic <project>` and start the JVM with the agent.';

export async function errorsCommand(project: string, idText: string | undefined, opts: RuntimeOptions, ctx: Context): Promise<number> {
  const id = parseId(idText, 'error');
  if (id !== undefined) {
    const e = await call(ctx, 'runtime_error', { project, error_id: id });
    if (opts.json) process.stdout.write(`${JSON.stringify(e, null, 2)}\n`);
    else process.stdout.write(`${errorReport(e).join('\n')}\n`);
    return ExitCode.Success;
  }
  const res = await call(ctx, 'runtime_errors', { project, limit: parseLimit(opts.limit) });
  if (opts.json) {
    process.stdout.write(`${JSON.stringify(res, null, 2)}\n`);
    return ExitCode.Success;
  }
  if (!res.store || res.errors.length === 0) {
    process.stdout.write(`${res.store ? 'No exceptions have been observed.' : NOTHING}\n`);
    return ExitCode.Success;
  }
  process.stdout.write(`${chalk.bold(`Exceptions observed at runtime (${res.total}), most recent first`)}\n`);
  for (const e of res.errors) {
    const color = e.state === 'UNCAUGHT' || e.state === 'ESCAPED' ? chalk.red : e.state === 'UNKNOWN' ? chalk.yellow : (s: string) => s;
    const top = e.top_app_frame ? `  at ${shortClass(e.top_app_frame.class)}.${e.top_app_frame.method}:${e.top_app_frame.line}` : '';
    process.stdout.write(`${String(e.id).padStart(5)}  ${color(e.state.padEnd(8))} ${e.exception}${e.message ? `: ${e.message}` : ''}${top}  x${e.occurrences}\n`);
  }
  process.stdout.write(chalk.dim(`\nStates: ${Object.entries(STATE_TEXT).map(([k, v]) => `${k} ${v}`).join('; ')}.\n`));
  process.stdout.write(chalk.dim('Details: proj errors <project> <id>\n'));
  return ExitCode.Success;
}

export async function tracesCommand(project: string, idText: string | undefined, opts: RuntimeOptions, ctx: Context): Promise<number> {
  const id = parseId(idText, 'trace');
  if (id !== undefined) {
    const t = await call(ctx, 'runtime_trace', { project, trace_id: id });
    if (opts.json) {
      process.stdout.write(`${JSON.stringify(t, null, 2)}\n`);
    } else {
      for (const l of traceLines(t)) process.stdout.write(`${l.text}\n`);
    }
    return ExitCode.Success;
  }
  const res = await call(ctx, 'runtime_traces', { project, limit: parseLimit(opts.limit) });
  if (opts.json) {
    process.stdout.write(`${JSON.stringify(res, null, 2)}\n`);
    return ExitCode.Success;
  }
  if (!res.store || res.traces.length === 0) {
    process.stdout.write(`${res.store ? 'No traces have been recorded.' : NOTHING}\n`);
    return ExitCode.Success;
  }
  process.stdout.write(`${chalk.bold(`Observed traces (${res.total}), newest first`)}\n`);
  for (const t of res.traces) {
    const flags = `${t.errors > 0 ? `  ${t.errors} error(s)` : ''}${t.status !== 'complete' ? `  [${t.status}]` : ''}`;
    process.stdout.write(`${String(t.id).padStart(5)}  ${traceLabel(t)}  [${t.thread}]  ${t.nodes} methods  ${durationText(t.duration_us)}${flags}\n`);
  }
  process.stdout.write(chalk.dim('Call tree: proj traces <project> <id>\n'));
  return ExitCode.Success;
}

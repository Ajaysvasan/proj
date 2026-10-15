// proj start|stop|status dynamic [project]: runtime tracing of Java applications.
import chalk from 'chalk';

import { CliError, type Context } from '../context.js';
import { ensureService, request, runningService, savedMonitors } from '../dynamic/service.js';
import { ExitCode } from '../exit-codes.js';

export interface StartOptions {
  pid?: string;
  match?: string;
  include?: string[];
  json?: boolean;
}

export interface StatusOptions {
  json?: boolean;
}

function requireDynamic(what: string, verb: string): void {
  if (what !== 'dynamic') {
    throw new CliError(`unknown target "${what}": the only one is "dynamic" (proj ${verb} dynamic [project])`, ExitCode.UserError);
  }
}

function serviceFailure(err: unknown): CliError {
  if (err instanceof CliError) return err;
  return new CliError(`the dynamic service failed: ${(err as Error).message}`, ExitCode.InternalError);
}

const STATE_COLOR: Record<string, (s: string) => string> = {
  IDLE: chalk.yellow,
  CAPTURING: chalk.green,
  ATTACHED: chalk.green,
  ATTACHING: chalk.cyan,
  TARGET_DISCOVERED: chalk.cyan,
  FLUSHING: chalk.cyan,
  STOPPED: chalk.dim,
};

function coverage(c: string): string {
  if (c === 'complete') return 'recording since JVM startup';
  if (c === 'attached_late') return 'attached to a running JVM: startup before the attach is not recorded';
  return 'recording was interrupted or incomplete';
}

/** Printable monitor status: state, sessions, the agent option, recent transitions. */
export function describeMonitor(m: any, transitions = 6): string[] {
  const color = STATE_COLOR[m.state] ?? ((s: string) => s);
  const lines = [`${chalk.bold(m.project)}  ${color(m.state)}  since ${m.since}`];
  lines.push(`  application packages  ${m.include}`);
  for (const s of m.sessions ?? []) lines.push(`  JVM pid ${s.pid}  ${coverage(s.startup_coverage)}`);
  if (m.pid) lines.push(`  attach target  pid ${m.pid}`);
  if (m.match) lines.push(`  attach rule  JVMs whose command line contains "${m.match}"`);
  if (m.last_error) lines.push(chalk.red(`  last error  ${m.last_error}`));
  lines.push('  to record from JVM startup, start the JVM with:');
  lines.push(`    ${m.agent_option.includes(' ') ? `"${m.agent_option}"` : m.agent_option}`);
  const history: any[] = m.history ?? [];
  if (transitions > 0 && history.length > 0) {
    lines.push('  recent transitions:');
    for (const h of history.slice(-transitions)) lines.push(chalk.dim(`    ${h.at}  ${h.state.padEnd(17)} ${h.detail}`));
  }
  return lines;
}

export async function startCommand(what: string, project: string | undefined, opts: StartOptions, ctx: Context): Promise<number> {
  requireDynamic(what, 'start');
  let pid: number | undefined;
  if (opts.pid !== undefined) {
    pid = Number(opts.pid);
    if (!Number.isInteger(pid) || pid < 1) throw new CliError(`invalid --pid "${opts.pid}"`, ExitCode.UserError);
  }
  if ((pid !== undefined || opts.match) && !project) {
    throw new CliError('--pid and --match need a project: proj start dynamic <project> --pid <pid>', ExitCode.UserError);
  }
  let res: any;
  try {
    const info = await ensureService(ctx);
    res = await request(info, { op: 'start', project, pid, match: opts.match, include: opts.include }, 20000);
  } catch (err) {
    throw serviceFailure(err);
  }
  if (opts.json) {
    process.stdout.write(`${JSON.stringify(res, null, 2)}\n`);
  } else if (!ctx.quiet) {
    for (const r of res.results ?? []) {
      if (!r.ok) {
        process.stderr.write(`${chalk.red('proj:')} ${r.project}: ${r.error?.message ?? 'not started'}\n`);
        continue;
      }
      const m = (res.monitors ?? []).find((x: any) => x.project === r.project);
      process.stdout.write(`${r.already ? 'Already monitoring' : 'Monitoring'} ${r.project}\n`);
      if (m) process.stdout.write(`${describeMonitor(m, r.already ? 4 : 0).slice(1).join('\n')}\n`);
    }
    if (!res.ok && res.error && (res.results ?? []).length === 0) process.stderr.write(`${chalk.red('proj:')} ${res.error.message}\n`);
  }
  if (res.ok) return ExitCode.Success;
  const code = res.error?.code;
  return code === 'internal_error' ? ExitCode.InternalError : ExitCode.UserError;
}

export async function stopCommand(what: string, project: string | undefined, opts: StatusOptions, ctx: Context): Promise<number> {
  requireDynamic(what, 'stop');
  const info = await runningService(ctx.dataDir);
  if (!info) {
    if (opts.json) process.stdout.write(`${JSON.stringify({ ok: true, running: false, stopped: [] })}\n`);
    else if (!ctx.quiet) process.stdout.write('Dynamic monitoring is not running.\n');
    return ExitCode.Success;
  }
  let res: any;
  try {
    res = await request(info, { op: 'stop', project }, 30000);
  } catch (err) {
    throw serviceFailure(err);
  }
  if (opts.json) {
    process.stdout.write(`${JSON.stringify(res, null, 2)}\n`);
  } else if (!ctx.quiet) {
    const stopped: string[] = res.stopped ?? [];
    if (stopped.length > 0) process.stdout.write(`Stopped monitoring ${stopped.join(', ')}; the applications keep running.\n`);
    else process.stdout.write(`${res.note ?? 'Nothing was being monitored.'}\n`);
    const left: any[] = res.monitors ?? [];
    if (left.length > 0) process.stdout.write(`Still monitoring: ${left.map((m) => m.project).join(', ')}\n`);
  }
  return ExitCode.Success;
}

export async function statusCommand(what: string, project: string | undefined, opts: StatusOptions, ctx: Context): Promise<number> {
  requireDynamic(what, 'status');
  const info = await runningService(ctx.dataDir);
  if (!info) {
    const saved = savedMonitors(ctx.dataDir);
    if (opts.json) {
      process.stdout.write(`${JSON.stringify({ running: false, saved_monitors: saved }, null, 2)}\n`);
    } else {
      process.stdout.write('Dynamic monitoring is not running.\n');
      if (saved.length > 0) {
        process.stdout.write(`It was monitoring ${saved.map((m) => m.project).join(', ')}; \`proj start dynamic\` resumes that.\n`);
      }
    }
    return ExitCode.Success;
  }
  let res: any;
  try {
    res = await request(info, { op: 'status', project }, 10000);
  } catch (err) {
    throw serviceFailure(err);
  }
  if (opts.json) {
    process.stdout.write(`${JSON.stringify({ running: true, ...res }, null, 2)}\n`);
    return ExitCode.Success;
  }
  const s = res.service ?? {};
  process.stdout.write(`Dynamic service: pid ${s.pid}, 127.0.0.1:${s.port}, since ${s.started_at}\n`);
  const monitors: any[] = res.monitors ?? [];
  if (monitors.length === 0) {
    process.stdout.write(project ? `${project} is not being monitored.\n` : 'No project is being monitored.\n');
    return ExitCode.Success;
  }
  for (const m of monitors) process.stdout.write(`\n${describeMonitor(m).join('\n')}\n`);
  return ExitCode.Success;
}

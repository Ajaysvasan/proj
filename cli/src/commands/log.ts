import chalk from 'chalk';

import { readAppLog } from '../applog.js';
import { CliError, type Context, engineOptions, processError, responseError } from '../context.js';
import { runEngine } from '../engine/client.js';
import { ExitCode } from '../exit-codes.js';
import { colorStatus, formatCount, formatDuration, truncate } from '../format.js';

export interface LogOptions {
  run?: string;
  limit?: string;
  json?: boolean;
  diagnostics?: string;
}

async function call(ctx: Context, op: string, args: Record<string, unknown>) {
  let res;
  try {
    res = await runEngine(op, args, engineOptions(ctx));
  } catch (err) {
    throw processError(ctx, op, err);
  }
  if (res.status !== 'ok' && res.status !== 'partial') throw responseError(ctx, op, res);
  return res.result;
}

function parsePositive(value: string | undefined, name: string, def: number): number {
  if (value === undefined) return def;
  const n = Number(value);
  if (!Number.isInteger(n) || n < 1) throw new CliError(`invalid ${name} "${value}"`, ExitCode.UserError);
  return n;
}

export async function logCommand(project: string | undefined, opts: LogOptions, ctx: Context): Promise<number> {
  if (opts.run !== undefined) return showRun(parsePositive(opts.run, '--run', 1), opts, ctx);
  const limit = parsePositive(opts.limit, '--limit', 20);
  const args: Record<string, unknown> = { limit };
  if (project) args.project = project;
  const result = await call(ctx, 'list_runs', args);
  const appErrors = readAppLog(ctx.dataDir, 5, 'WARN');
  if (opts.json) {
    process.stdout.write(JSON.stringify({ runs: result.runs, application_errors: appErrors }, null, 2) + '\n');
    return ExitCode.Success;
  }
  const runs = result.runs as any[];
  if (runs.length === 0) {
    process.stdout.write(project ? `No analysis runs recorded for ${project}.\n` : 'No analysis runs recorded yet.\n');
  } else {
    const header = ['RUN', 'PROJECT', 'STARTED (UTC)', 'DURATION', 'STATUS', 'FILES', 'SYMBOLS', 'RELS', 'UNRESOLVED', 'PARSE ERR', 'WARN'];
    const rows = runs.map((r) => [
      String(r.id),
      truncate(r.project, 20),
      String(r.started_at).replace('T', ' ').slice(0, 19),
      formatDuration(r.duration_ms),
      r.status,
      `${formatCount(r.counts.files_parsed)}/${formatCount(r.counts.files_discovered)}`,
      formatCount(r.counts.symbols),
      formatCount(r.counts.relationships),
      formatCount(r.counts.unresolved_refs),
      formatCount(r.counts.parse_errors),
      formatCount(r.counts.warnings),
    ]);
    const widths = header.map((h, i) => Math.max(h.length, ...rows.map((row) => row[i]!.length)));
    const fmt = (cells: string[], color = false) =>
      cells.map((c, i) => (color && i === 4 ? colorStatus(c) + ' '.repeat(widths[i]! - c.length) : c.padEnd(widths[i]!))).join('  ');
    process.stdout.write(chalk.bold(fmt(header)) + '\n');
    for (const row of rows) process.stdout.write(fmt(row, true) + '\n');
    const failed = runs.filter((r) => r.status === 'failed' || r.status === 'interrupted');
    for (const r of failed) {
      process.stdout.write(chalk.red(`  run ${r.id}: ${r.error_message ?? r.status}\n`));
    }
    process.stdout.write(chalk.dim(`\nDetails for a run: proj log --run <RUN>\n`));
  }
  if (appErrors.length > 0) {
    process.stdout.write(chalk.bold('\nRecent application warnings and errors (not source-analysis problems):\n'));
    for (const e of appErrors) {
      process.stdout.write(`  ${String(e.ts).replace('T', ' ').slice(0, 19)} ${colorStatus(e.level.toLowerCase())} [${e.source}/${e.component ?? '-'}] ${e.message}\n`);
    }
  }
  return ExitCode.Success;
}

async function showRun(runId: number, opts: LogOptions, ctx: Context): Promise<number> {
  const diagLimit = parsePositive(opts.diagnostics, '--diagnostics', 50);
  const run = await call(ctx, 'get_run', { run_id: runId, diag_limit: diagLimit });
  if (opts.json) {
    process.stdout.write(JSON.stringify(run, null, 2) + '\n');
    return ExitCode.Success;
  }
  const c = run.counts;
  const s = run.stats ?? {};
  const out: string[] = [];
  out.push(chalk.bold(`Run ${run.id} — ${run.project}`));
  out.push(`  Status          ${colorStatus(run.status)}`);
  out.push(`  Started         ${run.started_at}`);
  out.push(`  Finished        ${run.finished_at ?? '-'}`);
  out.push(`  Duration        ${formatDuration(run.duration_ms)}`);
  if (s.timing_ms) {
    out.push(
      `  Phases          scan+parse ${formatDuration(s.timing_ms.scan_and_parse)}, resolve ${formatDuration(s.timing_ms.resolve)}, ` +
        `finalize ${formatDuration(s.timing_ms.finalize)}`,
    );
  }
  out.push(`  Tool / schema   ${run.tool_version} / v${run.index_schema_version}`);
  out.push(
    `  Files           ${formatCount(c.files_discovered)} discovered, ${formatCount(c.files_parsed)} parsed, ${formatCount(c.files_skipped)} skipped`,
  );
  out.push(`  Symbols         ${formatCount(c.symbols)}`);
  out.push(`  Relationships   ${formatCount(c.relationships)}`);
  out.push(`  Unresolved refs ${formatCount(c.unresolved_refs)}`);
  out.push(`  Parse errors    ${formatCount(c.parse_errors)}`);
  out.push(`  Warnings        ${formatCount(c.warnings)}`);
  out.push(`  Fatal errors    ${formatCount(c.fatal_errors)}`);
  if (s.peak_rss_kib) out.push(`  Peak memory     ${formatCount(Math.round(s.peak_rss_kib / 1024))} MiB`);
  if (run.error_message) out.push(chalk.red(`  Error           ${run.error_message}`));
  process.stdout.write(out.join('\n') + '\n');

  const diags = run.diagnostics as any[];
  const internal = diags.filter((d) => d.category === 'internal');
  const analysis = diags.filter((d) => d.category !== 'internal');
  const print = (title: string, items: any[]) => {
    if (items.length === 0) return;
    process.stdout.write(chalk.bold(`\n${title}\n`));
    for (const d of items) {
      const where = d.path ? `${d.path}${d.line ? ':' + d.line : ''} ` : '';
      process.stdout.write(`  ${colorStatus(d.level).padEnd(5)} ${chalk.dim(`[${d.category}/${d.code}]`)} ${where}${d.message}\n`);
    }
  };
  print('Internal (application) problems:', internal);
  print('Source-analysis diagnostics:', analysis);
  if (diags.length === 0) process.stdout.write(chalk.dim('\nNo diagnostics were recorded for this run.\n'));
  const shown = diags.length;
  if (run.diagnostics_total > shown) {
    process.stdout.write(
      chalk.dim(
        `\nShowing ${shown} of ${run.diagnostics_total} diagnostics` +
          (run.diagnostics_dropped > 0 ? ` (${run.diagnostics_dropped} were not stored: per-run storage is bounded)` : '') +
          `. Use --diagnostics <N> to see more stored entries.\n`,
      ),
    );
  }
  return ExitCode.Success;
}

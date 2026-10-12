import { resolve } from 'node:path';

import chalk from 'chalk';

import { CliError, type Context, engineOptions, processError, responseError } from '../context.js';
import { runEngine } from '../engine/client.js';
import type { EngineResponse } from '../engine/protocol.js';
import { ExitCode } from '../exit-codes.js';
import { colorStatus, formatBytes, formatCount, formatDuration } from '../format.js';

export interface AddOptions {
  reanalyze?: boolean;
  exclude?: string[];
  includeDefault?: string[];
  defaultExcludes?: boolean;
  gitignore?: boolean;
  maxFileSize?: string;
  maxFiles?: string;
  json?: boolean;
}

const NAME_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

/** Parses "512", "512k", "2m" into bytes. */
export function parseSize(value: string): number {
  const m = /^(\d+)\s*([kKmM]?)[iI]?[bB]?$/.exec(value.trim());
  if (!m) throw new CliError(`invalid size "${value}" (use bytes, or a k/m suffix)`, ExitCode.UserError);
  const n = Number(m[1]);
  const unit = (m[2] ?? '').toLowerCase();
  return unit === 'k' ? n * 1024 : unit === 'm' ? n * 1024 * 1024 : n;
}

function analysisOptions(opts: AddOptions): Record<string, unknown> | undefined {
  const o: Record<string, unknown> = {};
  if (opts.exclude && opts.exclude.length > 0) o.exclude = opts.exclude;
  if (opts.includeDefault && opts.includeDefault.length > 0) o.include_default_excluded = opts.includeDefault;
  if (opts.defaultExcludes === false) o.default_excludes = false;
  if (opts.gitignore === false) o.use_gitignore = false;
  if (opts.maxFileSize) o.max_file_size = parseSize(opts.maxFileSize);
  if (opts.maxFiles) {
    const n = Number(opts.maxFiles);
    if (!Number.isInteger(n) || n < 1) throw new CliError(`invalid --max-files "${opts.maxFiles}"`, ExitCode.UserError);
    o.max_files = n;
  }
  return Object.keys(o).length > 0 ? o : undefined;
}

function line(label: string, value: string): string {
  return `  ${chalk.bold(label.padEnd(18))} ${value}`;
}

export function exitCodeForAnalysis(status: string): number {
  switch (status) {
    case 'success':
      return ExitCode.Success;
    case 'partial':
      return ExitCode.Partial;
    case 'no_supported_files':
      return ExitCode.NoSupportedFiles;
    default:
      return ExitCode.InternalError;
  }
}

export async function addCommand(name: string, path: string, opts: AddOptions, ctx: Context): Promise<number> {
  if (!NAME_RE.test(name)) {
    throw new CliError(
      `invalid project name "${name}": use 1-64 letters, digits, '.', '_' or '-', starting with a letter or digit`,
      ExitCode.UserError,
    );
  }
  const args: Record<string, unknown> = { name, path: resolve(path), reanalyze: Boolean(opts.reanalyze) };
  const options = analysisOptions(opts);
  if (options) args.options = options;

  if (!ctx.quiet && !opts.json) process.stderr.write(chalk.dim(`Analyzing ${name} (${args.path as string})...\n`));
  let res;
  try {
    res = await runEngine('add_project', args, engineOptions(ctx));
  } catch (err) {
    throw processError(ctx, 'add_project', err);
  }
  if (res.status !== 'ok' && res.status !== 'partial') {
    const err = responseError(ctx, 'add_project', res);
    if (res.status === 'analysis_failed' && res.error?.details && typeof res.error.details === 'object') {
      const details = res.error.details as { previous_index_kept?: boolean; run_id?: number };
      if (details.previous_index_kept) err.message += '\nThe previous index was kept unchanged.';
      if (details.run_id) err.message += `\nDetails: proj log ${name} --run ${details.run_id}`;
    }
    throw err;
  }
  return reportAnalysis(res, opts.json ?? false, ctx);
}

/** Prints an analysis or refresh summary and returns the command's exit code. */
export async function reportAnalysis(res: EngineResponse, json: boolean, ctx: Context): Promise<number> {
  const r = res.result;
  const code = exitCodeForAnalysis(r.status);
  if (json) {
    process.stdout.write(JSON.stringify({ ...r, diagnostics: res.diagnostics }, null, 2) + '\n');
    return code;
  }
  if (ctx.quiet) return code;

  const stats = r.stats ?? {};
  const files = stats.files ?? {};
  const res2 = stats.resolution ?? {};
  const out: string[] = [];
  const verb = r.mode === 'refresh' ? 'Refresh' : 'Analysis';
  const headline =
    r.status === 'success'
      ? chalk.green(`${verb} complete`)
      : r.status === 'partial'
        ? chalk.yellow(`${verb} complete with problems (partial)`)
        : chalk.yellow('No supported source files found');
  out.push(`${headline}`);
  out.push(line('Project', r.project));
  out.push(line('Root', r.root_path));
  out.push(line('Status', colorStatus(r.status)));
  out.push(line('Duration', formatDuration(r.duration_ms)));
  const rf = stats.refresh;
  if (rf) {
    out.push(
      line(
        'Changes',
        `${formatCount(rf.changed_files)} changed, ${formatCount(rf.added_files)} added, ${formatCount(rf.removed_files)} removed, ` +
          `${formatCount(rf.unchanged_files)} unchanged (re-parsed only changed and added files)`,
      ),
    );
    out.push(
      line(
        'Re-resolved',
        rf.full_reresolve
          ? `all relationships (${formatCount(rf.affected_files)} files; most files were affected)`
          : `${formatCount(rf.relationships_reset)} relationships in ${formatCount(rf.affected_files)} affected files`,
      ),
    );
  }
  out.push(
    line(
      'Files',
      `${formatCount(files.discovered)} discovered, ${formatCount(files.parsed)} parsed, ` +
        `${formatCount(files.with_syntax_errors)} with syntax errors, ${formatCount(files.parse_failed)} failed, ` +
        `${formatCount(files.skipped)} skipped, ${formatCount(files.unsupported)} unsupported, ${formatCount(files.excluded)} excluded`,
    ),
  );
  if (files.by_language && Object.keys(files.by_language).length > 0) {
    out.push(line('Languages', Object.entries(files.by_language).map(([k, v]) => `${k} ${v}`).join(', ')));
  }
  out.push(line('Symbols', formatCount(stats.symbols)));
  out.push(
    line(
      'Relationships',
      `${formatCount(r.counts?.relationships)} (${formatCount(stats.call_edges)} calls, ${formatCount(stats.dependencies)} file dependencies)`,
    ),
  );
  const calls = res2.calls ?? {};
  if (calls.total) {
    out.push(
      line(
        'Call resolution',
        `${formatCount(calls.resolved)} resolved, ${formatCount(calls.external)} external, ${formatCount(calls.ambiguous)} ambiguous, ` +
          `${formatCount(calls.heuristic)} heuristic, ${formatCount(calls.unresolved)} unresolved`,
      ),
    );
  }
  out.push(line('Routes', formatCount(stats.routes)));
  out.push(line('Entry points', formatCount(stats.entry_points)));
  out.push(line('Exception sites', `${formatCount(stats.exception_sites)} throw/raise, ${formatCount(stats.exception_handlers)} handlers`));
  out.push(line('Unresolved refs', formatCount(r.counts?.unresolved_refs)));
  out.push(line('Parse problems', `${formatCount(r.counts?.parse_errors)} files, ${formatCount(r.counts?.warnings)} warnings`));
  if (stats.peak_rss_kib) out.push(line('Peak memory', formatBytes(stats.peak_rss_kib * 1024)));
  out.push(line('Index', r.index_path));
  for (const d of res.diagnostics ?? []) out.push(chalk.yellow(`  note: ${d.message}`));
  process.stdout.write(out.join('\n') + '\n');

  if (r.counts?.parse_errors > 0 || r.counts?.warnings > 0) {
    // Show the first few diagnostics; the full list is in `proj log`.
    try {
      const run = await runEngine('get_run', { run_id: r.run_id, diag_limit: 5 }, engineOptions(ctx, { onStderr: undefined }));
      if (run.status === 'ok' && run.result.diagnostics.length > 0) {
        process.stdout.write(chalk.bold('\n  First diagnostics:\n'));
        for (const d of run.result.diagnostics) {
          const where = d.path ? `${d.path}${d.line ? ':' + d.line : ''}` : '';
          process.stdout.write(`    ${colorStatus(d.level)} ${chalk.dim(d.code)} ${where} ${d.message}\n`);
        }
        process.stdout.write(chalk.dim(`  Full details: proj log ${r.project} --run ${r.run_id}\n`));
      }
    } catch {
      // the summary above is complete without these extras
    }
  }
  return code;
}


import chalk from 'chalk';
import { render } from 'ink';
import React from 'react';

import { CliError, type Context, engineOptions, processError, responseError } from '../context.js';
import { EngineSession, runEngine } from '../engine/client.js';
import { ExitCode } from '../exit-codes.js';
import { colorStatus, formatCount, glyph } from '../format.js';
import { App } from '../ui/App.js';
import { describeRow, layoutTree } from '../ui/tree.js';
import { handlerLabel } from '../ui/views.js';

export interface ShowOptions {
  tui?: boolean;
  check?: boolean;
}

async function call(ctx: Context, op: string, args: Record<string, unknown>) {
  let res;
  try {
    res = await runEngine(op, args, engineOptions(ctx, { timeoutMs: 60000 }));
  } catch (err) {
    throw processError(ctx, op, err);
  }
  if (res.status !== 'ok' && res.status !== 'partial') throw responseError(ctx, op, res);
  return res.result;
}

/** Plain-text overview used when no interactive terminal is available. */
async function printSummary(ctx: Context, project: string, status: any): Promise<void> {
  const idx = status.index;
  const c = idx.counts;
  const out: string[] = [];
  out.push(chalk.bold(`proj · ${project}`) + ` [${colorStatus(idx.analysis_status)}]`);
  out.push(`  Root          ${status.project.root_path}`);
  out.push(`  Indexed       ${idx.indexed_at} (run ${idx.run_id}, tool ${idx.tool_version}, schema v${idx.schema_version})`);
  out.push(`  Files         ${formatCount(c.source_files)} source files of ${formatCount(c.files)}`);
  out.push(`  Languages     ${Object.entries(idx.languages).map(([k, v]) => `${k} ${v}`).join(', ') || 'none'}`);
  out.push(`  Symbols       ${formatCount(c.symbols)}`);
  out.push(`  Relationships ${formatCount(c.relationships)} (${formatCount(c.call_edges)} calls, ${formatCount(c.dependencies)} file dependencies)`);
  out.push(`  Unresolved    ${formatCount(c.unresolved_edges)} call/registration edges`);
  out.push(`  Routes        ${formatCount(c.routes)}`);
  out.push(`  Exceptions    ${formatCount(c.exception_sites)} explicit throw/raise sites`);
  const entries = await call(ctx, 'entry_points', { project, limit: 10 });
  out.push(chalk.bold(`\n  Entry points (${entries.entry_points.length}${entries.entry_points.length >= 10 ? '+' : ''})`));
  if (entries.entry_points.length === 0) out.push(`    ${entries.message}`);
  for (const e of entries.entry_points) out.push(`    ${e.kind.padEnd(15)} ${e.path}:${e.line}  ${e.reason}`);
  const routes = await call(ctx, 'routes', { project, limit: 20 });
  out.push(chalk.bold(`\n  Routes (${routes.routes.length}${routes.has_more ? '+' : ''})`));
  for (const r of routes.routes) {
    out.push(
      `    ${r.method.padEnd(7)} ${r.full_path}  -> ${handlerLabel(r.handler?.qualified_name, r.handler?.kind, r.handler_name)} [${r.handler_status}]  ${r.file}:${r.line}`,
    );
  }
  out.push(chalk.dim('\n  The interactive explorer needs a terminal (TTY). Run `proj show` in a terminal to search and navigate.'));
  process.stdout.write(out.join('\n') + '\n');
}

/** Routes matching an endpoint such as "GET /api/users/42", best first. */
async function findEndpoint(ctx: Context, project: string, endpoint: string): Promise<any[]> {
  const res = await call(ctx, 'search', { project, query: endpoint, kinds: ['route'], limit: 20 });
  const routes: any[] = res.routes ?? [];
  if (routes.length === 0) {
    throw new CliError(
      `no API endpoint of '${project}' matches '${endpoint}'.\nList the routes with: proj show ${project} --no-tui (or press r in the explorer)`,
      ExitCode.UserError,
    );
  }
  return routes;
}

/** Text form of a route: registration, handler, middleware, startup path and call flow. */
async function printEndpoint(ctx: Context, project: string, endpoint: string, matches: any[]): Promise<void> {
  const best = matches[0];
  const flow = await call(ctx, 'route_flow', { project, route_id: best.id, max_depth: 1, max_nodes: 1 });
  const r = flow.route;
  const h = r.handler.symbol;
  const out: string[] = [];
  out.push(chalk.bold(`${r.method} ${r.full_path}  ${glyph.arrow} ${handlerLabel(h?.qualified_name, h?.kind, r.handler.text ?? r.handler.name)}`) + `  [${colorStatus(r.handler.status)}]`);
  out.push(`  registered  ${r.registration.path}:${r.registration.line} on ${r.registration.registrar}  (${r.framework}, ${r.confidence})`);
  if (r.path !== r.full_path) out.push(`  declared as ${r.path}`);
  out.push(h ? `  handler     ${h.qualified_name}  ${h.path}:${h.line}` : `  handler     not resolved: ${r.handler.text ?? '?'}`);
  if (r.handler.note) out.push(chalk.dim(`              ${r.handler.note}`));
  out.push(
    r.middleware.length > 0
      ? `  middleware  ${r.middleware.map((m: any) => `${m.scope}:${m.text}${m.status === 'resolved' ? '' : ` [${m.status}]`}`).join(` ${glyph.arrow} `)}`
      : '  middleware  none recognized',
  );
  const st = flow.startup;
  if (st?.found) out.push(`  startup     ${st.path.map((p: any) => p.symbol?.qualified_name ?? '?').join(` ${glyph.arrow} `)}  (entry: ${st.entry_kind})`);
  else if (st) out.push(`  startup     ${st.message}`);
  if (h) {
    const tree = await call(ctx, 'traverse', { project, symbol_id: h.id, direction: 'forward', max_depth: 4, max_nodes: 200, include_unresolved: true });
    out.push(chalk.bold('\n  Call flow from the handler (depth 4):'));
    const rows = layoutTree(tree.nodes ?? []);
    for (const row of rows.slice(0, 80)) out.push(`    ${row.prefix}${describeRow(row)}`);
    if (rows.length > 80 || tree.truncated) out.push(chalk.dim('    ... (more in the interactive explorer)'));
  }
  if (matches.length > 1) {
    out.push(chalk.bold(`\n  Other matches for '${endpoint}' (${matches.length - 1}):`));
    for (const m of matches.slice(1, 11)) {
      out.push(`    ${m.method.padEnd(7)} ${m.full_path}  ${glyph.arrow} ${handlerLabel(m.handler_qualified, m.handler_symbol_kind, m.handler_name)}  ${m.file}:${m.line}`);
    }
  }
  process.stdout.write(out.join('\n') + '\n');
}

/** Runs the index integrity check (dangling references, pending rows, schema). */
async function checkIndex(ctx: Context, project: string): Promise<number> {
  const r = await call(ctx, 'check_index', { project });
  process.stdout.write(`Index integrity for ${project} (schema v${r.schema_version}): ${r.ok ? chalk.green('ok') : chalk.red('problems found')}\n`);
  for (const c of r.checks) {
    const mark = c.problems === 0 ? chalk.green('ok ') : chalk.red('BAD');
    process.stdout.write(`  ${mark} ${c.check.padEnd(30)} ${c.problems === 0 ? '' : `${c.problems} ${c.detail}`}\n`);
  }
  if (!r.ok) process.stdout.write(`Re-run the analysis to rebuild the index: proj add ${project} <path> --reanalyze\n`);
  return r.ok ? ExitCode.Success : ExitCode.InternalError;
}

export async function showCommand(project: string, endpointWords: string[], opts: ShowOptions, ctx: Context): Promise<number> {
  // Validates registration and that the index exists and is readable.
  const status = await call(ctx, 'project_status', { project });
  if (opts.check) return checkIndex(ctx, project);
  const endpoint = endpointWords.join(' ').trim();
  const matches = endpoint ? await findEndpoint(ctx, project, endpoint) : [];
  const interactive = opts.tui !== false && Boolean(process.stdin.isTTY) && Boolean(process.stdout.isTTY);
  if (!interactive) {
    if (endpoint) await printEndpoint(ctx, project, endpoint, matches);
    else await printSummary(ctx, project, status);
    return ExitCode.Success;
  }

  const session = new EngineSession(engineOptions(ctx, { onStderr: undefined, timeoutMs: 30000 }));
  const app = render(
    React.createElement(App, {
      session,
      project,
      summary: status.index,
      rootPath: status.project.root_path,
      // Back (Esc) from the route shows every endpoint that matched.
      initialRoute: endpoint ? matches[0].id : undefined,
      initialQuery: endpoint ? `/show ${endpoint}` : undefined,
    }),
    { exitOnCtrlC: true },
  );
  const stop = () => app.unmount();
  process.once('SIGTERM', stop);
  process.once('SIGHUP', stop);
  try {
    await app.waitUntilExit();
  } finally {
    process.off('SIGTERM', stop);
    process.off('SIGHUP', stop);
    await session.close();
  }
  return ExitCode.Success;
}

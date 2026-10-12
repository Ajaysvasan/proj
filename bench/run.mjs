#!/usr/bin/env node
// Benchmark runner: generates synthetic repositories of increasing size,
// analyzes each with proj-engine, and measures analysis time, peak resident
// memory, index size and query latencies.
//
//   node bench/run.mjs [--sizes 500,2000,8000] [--engine <path>] [--queries 30]
//                      [--max-rss-mib <N>] [--out results.json] [--keep]
//
// --max-rss-mib makes the run fail when any analysis exceeds that peak RSS
// (memory regression check). Results include hardware and tool versions.
import { execFileSync, spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { cpus, platform, release, tmpdir, totalmem } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..');

function parseArgs(argv) {
  const a = { sizes: [500, 2000, 8000], engine: process.env.PROJ_ENGINE || join(repo, 'build', 'engine', 'proj-engine'), queries: 30, maxRss: null, out: null, keep: false };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    if (k === '--sizes') a.sizes = argv[++i].split(',').map(Number);
    else if (k === '--engine') a.engine = resolve(argv[++i]);
    else if (k === '--queries') a.queries = Number(argv[++i]);
    else if (k === '--max-rss-mib') a.maxRss = Number(argv[++i]);
    else if (k === '--out') a.out = resolve(argv[++i]);
    else if (k === '--keep') a.keep = true;
    else throw new Error(`unknown option ${k}`);
  }
  return a;
}

function oneShot(engine, request) {
  const out = execFileSync(engine, [], { input: JSON.stringify(request), maxBuffer: 256 * 1024 * 1024 });
  return JSON.parse(out.toString().trim().split('\n').pop());
}

class Session {
  constructor(engine, dataDir) {
    this.dataDir = dataDir;
    this.child = spawn(engine, ['--serve'], { stdio: ['pipe', 'pipe', 'inherit'] });
    this.waiting = new Map();
    this.id = 0;
    createInterface({ input: this.child.stdout }).on('line', (line) => {
      const res = JSON.parse(line);
      const w = this.waiting.get(res.id);
      this.waiting.delete(res.id);
      w?.(res);
    });
  }
  request(op, args) {
    const id = ++this.id;
    return new Promise((r) => {
      this.waiting.set(id, r);
      this.child.stdin.write(JSON.stringify({ protocol: 1, id, op, args, options: { data_dir: this.dataDir } }) + '\n');
    });
  }
  close() {
    this.child.stdin.end();
  }
}

function percentile(sorted, p) {
  if (sorted.length === 0) return null;
  return sorted[Math.min(sorted.length - 1, Math.floor((p / 100) * sorted.length))];
}

async function timeQueries(session, label, n, make) {
  const times = [];
  let last;
  for (let i = 0; i < n; i++) {
    const { op, args } = make(i);
    const t0 = process.hrtime.bigint();
    last = await session.request(op, args);
    times.push(Number(process.hrtime.bigint() - t0) / 1e6);
    if (last.status !== 'ok' && last.status !== 'partial') throw new Error(`${label} failed: ${JSON.stringify(last.error)}`);
  }
  times.sort((x, y) => x - y);
  return { label, runs: n, median_ms: percentile(times, 50), p95_ms: percentile(times, 95), max_ms: times[times.length - 1], last };
}

function environment(engine) {
  let engineVersion = null;
  try {
    engineVersion = JSON.parse(execFileSync(engine, ['--version']).toString());
  } catch {
    // reported as null
  }
  let compiler = null;
  try {
    const cache = execFileSync('cmake', ['-L', '-N', join(dirname(engine))], { stdio: ['ignore', 'pipe', 'ignore'] }).toString();
    compiler = /CMAKE_CXX_COMPILER:\w+=(.*)/.exec(cache)?.[1] ?? null;
  } catch {
    // optional
  }
  let compilerVersion = null;
  try {
    compilerVersion = execFileSync(compiler ?? 'c++', ['--version']).toString().split('\n')[0];
  } catch {
    // optional
  }
  return {
    os: `${platform()} ${release()}`,
    cpu: cpus()[0]?.model ?? 'unknown',
    cores: cpus().length,
    memory_gib: Math.round((totalmem() / 1024 ** 3) * 10) / 10,
    node: process.version,
    engine: engineVersion,
    compiler: compilerVersion,
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!existsSync(args.engine)) throw new Error(`engine not found at ${args.engine}; build it first`);
  const env = environment(args.engine);
  const results = [];
  let failed = false;
  for (const size of args.sizes) {
    const work = mkdtempSync(join(tmpdir(), 'proj-bench-'));
    const src = join(work, 'repo');
    const dataDir = join(work, 'data');
    try {
      execFileSync(process.execPath, [join(here, 'generate.mjs'), src, '--files', String(size), '--seed', '42']);
      const t0 = process.hrtime.bigint();
      const add = oneShot(args.engine, { protocol: 1, id: 1, op: 'add_project', args: { name: 'bench', path: src }, options: { data_dir: dataDir } });
      const wall = Number(process.hrtime.bigint() - t0) / 1e6;
      if (!add.result) throw new Error(`analysis failed: ${JSON.stringify(add.error)}`);
      const s = add.result.stats;
      const session = new Session(args.engine, dataDir);
      const q = [];
      q.push(await timeQueries(session, 'search substring "fn1_"', args.queries, (i) => ({ op: 'search', args: { project: 'bench', query: `fn${i}_`, limit: 20 } })));
      q.push(await timeQueries(session, 'search exact name', args.queries, (i) => ({ op: 'search', args: { project: 'bench', query: `fn${i * 7}_1`, exact: true, limit: 20 } })));
      q.push(await timeQueries(session, 'search route "GET /items0/42"', args.queries, () => ({ op: 'search', args: { project: 'bench', query: 'GET /items0/42', kinds: ['route'] } })));
      const hit = (await session.request('search', { project: 'bench', query: `fn${Math.floor(size * 0.7)}_3`, exact: true })).result.symbols[0];
      q.push(await timeQueries(session, 'traverse forward depth 6 (max 500 nodes)', args.queries, () => ({ op: 'traverse', args: { project: 'bench', symbol_id: hit.id, max_depth: 6, max_nodes: 500 } })));
      q.push(await timeQueries(session, 'traverse backward depth 4', args.queries, () => ({ op: 'traverse', args: { project: 'bench', symbol_id: hit.id, direction: 'backward', max_depth: 4, max_nodes: 500 } })));
      const route = (await session.request('routes', { project: 'bench', limit: 1 })).result.routes[0];
      q.push(await timeQueries(session, 'route_flow depth 4', args.queries, () => ({ op: 'route_flow', args: { project: 'bench', route_id: route.id, max_depth: 4 } })));
      session.close();
      const traverseNodes = q[3].last.result.nodes.length;
      const row = {
        files_requested: size,
        files_discovered: s.files.discovered,
        files_parsed: s.files.parsed + s.files.with_syntax_errors,
        files_skipped: s.files.skipped,
        status: add.result.status,
        analysis_ms: Math.round(add.result.duration_ms),
        wall_ms: Math.round(wall),
        phases_ms: Object.fromEntries(Object.entries(s.timing_ms).map(([k, v]) => [k, Math.round(v)])),
        peak_rss_mib: s.peak_rss_kib ? Math.round((s.peak_rss_kib / 1024) * 10) / 10 : null,
        index_mib: Math.round((statSync(add.result.index_path).size / 1024 ** 2) * 10) / 10,
        symbols: s.symbols,
        edges: s.graph_edges,
        dependencies: s.dependencies,
        routes: s.routes,
        traverse_nodes_returned: traverseNodes,
        queries: q.map(({ last, ...rest }) => ({ ...rest, median_ms: Math.round(rest.median_ms * 100) / 100, p95_ms: Math.round(rest.p95_ms * 100) / 100, max_ms: Math.round(rest.max_ms * 100) / 100 })),
      };
      results.push(row);
      if (args.maxRss !== null && row.peak_rss_mib !== null && row.peak_rss_mib > args.maxRss) {
        process.stderr.write(`FAIL: ${size} files used ${row.peak_rss_mib} MiB peak RSS (limit ${args.maxRss} MiB)\n`);
        failed = true;
      }
      if (traverseNodes > 500) {
        process.stderr.write(`FAIL: traversal returned ${traverseNodes} nodes despite max_nodes=500\n`);
        failed = true;
      }
      process.stderr.write(`done: ${size} files in ${row.analysis_ms} ms, peak ${row.peak_rss_mib} MiB\n`);
    } finally {
      if (!args.keep && basename(work).startsWith('proj-bench-')) rmSync(work, { recursive: true, force: true });
      else if (args.keep) process.stderr.write(`kept ${work}\n`);
    }
  }
  const report = { environment: env, generated_at: new Date().toISOString(), results };
  if (args.out) writeFileSync(args.out, JSON.stringify(report, null, 2) + '\n');
  // Markdown summary on stdout.
  const lines = [];
  lines.push(`Environment: ${env.os}, ${env.cpu} (${env.cores} threads), ${env.memory_gib} GiB RAM, Node ${env.node}, ${env.compiler ?? 'compiler unknown'}, proj-engine ${env.engine?.tool_version ?? '?'}`);
  lines.push('');
  lines.push('| files | parsed | analysis | peak RSS | index | symbols | edges | search (median/p95) | traverse d6 (median/p95) | route flow (median/p95) |');
  lines.push('|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|');
  for (const r of results) {
    const m = (i) => `${r.queries[i].median_ms} / ${r.queries[i].p95_ms} ms`;
    lines.push(`| ${r.files_discovered} | ${r.files_parsed} | ${(r.analysis_ms / 1000).toFixed(2)} s | ${r.peak_rss_mib} MiB | ${r.index_mib} MiB | ${r.symbols} | ${r.edges} | ${m(0)} | ${m(3)} | ${m(5)} |`);
  }
  process.stdout.write(lines.join('\n') + '\n');
  process.exitCode = failed ? 1 : 0;
}

main().catch((err) => {
  process.stderr.write(`benchmark failed: ${err.stack ?? err}\n`);
  process.exitCode = 1;
});

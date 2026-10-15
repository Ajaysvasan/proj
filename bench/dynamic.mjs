#!/usr/bin/env node
// Runtime tracing benchmark on the dynamic-java fixture (needs a JDK, the built engine, CLI and agent):
//
//   node bench/dynamic.mjs [--seconds 8] [--runs 3] [--traces 20000] [--idle 30] [--out results.json]
//
//   overhead     calls/s of the request path (Load mode, one long trace) without the agent, with the
//                agent recording, and with the agent loaded but on standby (project not monitored);
//                median of --runs runs each
//   requests     mean latency of --requests sequential HTTP requests (after 500 warm-up requests)
//                to the fixture server, without and with the agent recording
//   throughput   --traces short traces on a 4-thread pool: JVM time, service ingest time, service
//                peak RSS, store size
//   backpressure the same load with a 16-item agent queue: dropped items are counted and the session's
//                coverage is no longer "complete"
//   idle         the service's CPU time and RSS over --idle seconds while it waits for a JVM
import { execFileSync, spawn, spawnSync } from 'node:child_process';
import { Agent, get } from 'node:http';
import { existsSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { cpus, platform, release, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, '..');
const args = { seconds: 8, runs: 3, traces: 20000, requests: 3000, idle: 30, out: null };
for (let i = 2; i < process.argv.length; i++) {
  const k = process.argv[i];
  if (k === '--seconds') args.seconds = Number(process.argv[++i]);
  else if (k === '--runs') args.runs = Number(process.argv[++i]);
  else if (k === '--traces') args.traces = Number(process.argv[++i]);
  else if (k === '--requests') args.requests = Number(process.argv[++i]);
  else if (k === '--idle') args.idle = Number(process.argv[++i]);
  else if (k === '--out') args.out = resolve(process.argv[++i]);
  else throw new Error(`unknown option ${k}`);
}

const engine = process.env.PROJ_ENGINE || join(repo, 'build', 'engine', 'proj-engine');
const cliJs = join(repo, 'cli', 'dist', 'src', 'index.js');
const work = mkdtempSync(join(tmpdir(), 'proj-bench-dynamic-'));
const data = join(work, 'data');
const classes = join(work, 'classes');
const env = { ...process.env, PROJ_HOME: data, PROJ_ENGINE: engine, NO_COLOR: '1' };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function sources(dir) {
  return readdirSync(dir).flatMap((e) => {
    const p = join(dir, e);
    return statSync(p).isDirectory() ? sources(p) : p.endsWith('.java') ? [p] : [];
  });
}
const proj = (...a) => execFileSync(process.execPath, [cliJs, ...a], { env, encoding: 'utf8' });
const engineOp = (op, a) => JSON.parse(execFileSync(engine, [], { input: JSON.stringify({ op, args: a, options: { data_dir: data } }), encoding: 'utf8' })).result;
const java = (jvmArgs) => spawnSync('java', [...jvmArgs], { encoding: 'utf8', env });
const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

// CPU seconds and RSS of a process (Linux /proc; ps elsewhere).
function usage(pid) {
  if (platform() === 'linux') {
    const stat = readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' ');
    const ticks = Number(stat[11]) + Number(stat[12]);
    const status = readFileSync(`/proc/${pid}/status`, 'utf8');
    const kb = (k) => Number((new RegExp(`${k}:\\s+(\\d+)`).exec(status) ?? [0, 0])[1]);
    return { cpu_s: ticks / 100, rss_kib: kb('VmRSS'), peak_rss_kib: kb('VmHWM') };
  }
  const [rss, time] = execFileSync('ps', ['-o', 'rss=,time=', '-p', String(pid)], { encoding: 'utf8' }).trim().split(/\s+/);
  const [m, s] = time.split(':').map(Number);
  return { cpu_s: m * 60 + s, rss_kib: Number(rss), peak_rss_kib: null };
}

async function main() {
  execFileSync('javac', ['-nowarn', '-d', classes, ...sources(join(repo, 'fixtures', 'dynamic-java'))], { stdio: 'ignore' });
  engineOp('add_project', { name: 'shop', path: join(repo, 'fixtures', 'dynamic-java') });
  engineOp('add_project', { name: 'other', path: join(repo, 'fixtures', 'legacy-java') });
  proj('start', 'dynamic', 'other');  // keeps the service up while shop is not monitored
  const service = JSON.parse(readFileSync(join(data, 'dynamic', 'service.json'), 'utf8'));
  const status = () => JSON.parse(proj('status', 'dynamic', '--json'));
  proj('start', 'dynamic', 'shop');
  const agent = status().monitors.find((m) => m.project === 'shop').agent_option;
  const results = {
    host: { cpus: cpus().length, cpu: cpus()[0]?.model, platform: platform(), release: release(), java: spawnSync('java', ['-version'], { encoding: 'utf8' }).stderr.split('\n')[0] },
  };

  // 1. Overhead of the request path.
  const load = (extra) => JSON.parse(java([...extra, '-cp', classes, 'com.example.shop.ShopApp', 'load', String(args.seconds)]).stdout.trim().split('\n').pop());
  const rate = (extra) => median(Array.from({ length: args.runs }, () => {
    const r = load(extra);
    return r.calls / r.seconds;
  }));
  const baseline = rate([]);
  const recording = rate([agent]);
  proj('stop', 'dynamic', 'shop');
  const standby = rate([agent]);
  proj('start', 'dynamic', 'shop');
  results.overhead = {
    workload: `UserService.describe(id) in a loop for ${args.seconds} s (5 application methods per call), median of ${args.runs} runs`,
    calls_per_s: { no_agent: Math.round(baseline), recording: Math.round(recording), standby: Math.round(standby) },
    slowdown: { recording: +(baseline / recording).toFixed(2), standby: +(baseline / standby).toFixed(2) },
  };

  // 1b. Request latency on the fixture server (HTTP, sequential, keep-alive).
  const latency = async (extra) => {
    const portFile = join(work, `port-${Date.now()}`);
    // TCP_NODELAY: otherwise Nagle plus delayed ACKs add ~40 ms to every response and hide everything else.
    const jvm = spawn('java', [...extra, '-Dsun.net.httpserver.nodelay=true', '-cp', classes, 'com.example.shop.ShopApp', 'wait', portFile], { stdio: 'ignore', env });
    while (!existsSync(portFile)) await sleep(50);
    const port = Number(readFileSync(portFile, 'utf8'));
    const keepAlive = new Agent({ keepAlive: true, maxSockets: 1 });
    const one = (i) => new Promise((ok, fail) => get({ host: '127.0.0.1', port, path: `/users/${i}`, agent: keepAlive }, (r) => {
      r.resume();
      r.on('end', ok);
    }).on('error', fail));
    for (let i = 0; i < 500; i++) await one(i);
    const t = process.hrtime.bigint();
    for (let i = 0; i < args.requests; i++) await one(i);
    const us = Number(process.hrtime.bigint() - t) / 1000 / args.requests;
    keepAlive.destroy();
    jvm.kill('SIGKILL');
    return us;
  };
  const plainUs = await latency([]);
  const tracedUs = await latency([agent]);
  results.requests = {
    workload: `${args.requests} sequential GET /users/<id> (11 application methods each) after 500 warm-up requests`,
    mean_us: { no_agent: +plainUs.toFixed(1), recording: +tracedUs.toFixed(1) },
    added_us_per_request: +(tracedUs - plainUs).toFixed(1),
  };

  // 2. Throughput: many short traces.
  const tracesBefore = engineOp('runtime_traces', { project: 'shop', limit: 1 }).total;
  const t0 = Date.now();
  const run = JSON.parse(java([agent, '-cp', classes, 'com.example.shop.ShopApp', 'traces', String(args.traces)]).stdout.trim().split('\n').pop());
  const jvmDone = Date.now();
  // Stored when the count reaches what retention keeps (the newest 5000 traces), or stops growing.
  const expected = Math.min(tracesBefore + args.traces, 5000);
  let stored = 0;
  let last = -1;
  let stableSince = Date.now();
  while (Date.now() - jvmDone < 60000) {
    stored = engineOp('runtime_traces', { project: 'shop', limit: 1 }).total;
    if (stored >= expected) break;
    if (stored !== last) {
      last = stored;
      stableSince = Date.now();
    } else if (Date.now() - stableSince > 2000) {
      break;
    }
    await sleep(200);
  }
  const svc = usage(service.pid);
  const session = engineOp('runtime_sessions', { project: 'shop', limit: 1 }).sessions[0];
  const dropped = session.stats.dropped_items ?? 0;
  const seconds = (Date.now() - t0) / 1000;
  results.throughput = {
    traces: args.traces,
    jvm_ms: run.ms,
    dropped_items: dropped,
    delivered_traces: args.traces - dropped,
    until_stored_ms: Date.now() - t0,
    delivered_per_s: Math.round((args.traces - dropped) / seconds),
    stored_traces_after_retention: stored,
    service_peak_rss_mib: svc.peak_rss_kib ? +(svc.peak_rss_kib / 1024).toFixed(1) : null,
    store_mib: +(statSync(join(data, 'runtime', 'project-1.db')).size / 1048576).toFixed(1),
  };

  // 3. Backpressure: a tiny queue.
  const tiny = java([`${agent},queueItems=16`, '-cp', classes, 'com.example.shop.ShopApp', 'traces', String(args.traces)]);
  await sleep(1500);
  const s2 = engineOp('runtime_sessions', { project: 'shop', limit: 1 }).sessions[0];
  results.backpressure = {
    queue_items: 16,
    jvm_exit: tiny.status,
    dropped_items: s2.stats.dropped_items ?? 0,
    startup_coverage: s2.startup_coverage,
  };

  // 4. Idle: waiting for a JVM.
  await sleep(1000);
  const a = usage(service.pid);
  await sleep(args.idle * 1000);
  const b = usage(service.pid);
  results.idle = { seconds: args.idle, service_cpu_s: +(b.cpu_s - a.cpu_s).toFixed(3), service_rss_mib: +(b.rss_kib / 1024).toFixed(1) };

  proj('stop', 'dynamic');
  console.log(JSON.stringify(results, null, 2));
  if (args.out) writeFileSync(args.out, JSON.stringify(results, null, 2));
}

main()
  .catch((e) => {
    console.error(e);
    process.exitCode = 1;
  })
  .finally(() => {
    try {
      execFileSync(process.execPath, [cliJs, 'stop', 'dynamic'], { env, stdio: 'ignore' });
    } catch {
      // already stopped
    }
    if (existsSync(work) && work.includes('proj-bench-dynamic-')) rmSync(work, { recursive: true, force: true });
  });

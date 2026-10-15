// Runtime tracing end to end: the real `proj`, the dynamic service, the Java agent and the
// dynamic-java fixture running in real JVMs. Skipped without a JDK or the built agent.
import assert from 'node:assert/strict';
import { type ChildProcess, spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { get } from 'node:http';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';

import { locateAgentJar } from '../src/dynamic/service.js';
import { cleanupTemp, ENGINE, FIXTURES, runCli, tempDir } from './helpers.js';

const hasJdk = spawnSync('javac', ['-version']).status === 0 && spawnSync('java', ['-version']).status === 0;
const AGENT = locateAgentJar(ENGINE);
const skip = !hasJdk ? 'needs a JDK (java, javac) on PATH' : !AGENT ? 'needs the built agent (build/engine/agent/proj-agent.jar)' : false;

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function waitFor<T>(what: string, fn: () => T | undefined | null | false, timeoutMs = 20000): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const v = fn();
    if (v) return v;
    await sleep(100);
  }
  throw new Error(`timed out waiting for ${what}`);
}

function javaSources(dir: string): string[] {
  const out: string[] = [];
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) out.push(...javaSources(p));
    else if (p.endsWith('.java')) out.push(p);
  }
  return out;
}

function engine(dataDir: string, op: string, args: Record<string, unknown>): any {
  const r = spawnSync(ENGINE, [], { input: JSON.stringify({ op, args, options: { data_dir: dataDir } }), encoding: 'utf8' });
  const res = JSON.parse(r.stdout);
  assert.equal(res.status, 'ok', JSON.stringify(res.error));
  return res.result;
}

function httpGet(port: number, path: string): Promise<number> {
  return new Promise((resolve) => {
    get({ host: '127.0.0.1', port, path }, (res) => {
      res.resume();
      res.on('end', () => resolve(res.statusCode ?? 0));
    }).on('error', () => resolve(-1));
  });
}

describe('runtime tracing (proj start|stop|status dynamic, errors, traces)', { skip }, () => {
  let data: string;
  let classes: string;
  let env: Record<string, string>;
  let indexHash: string;
  const jvms: ChildProcess[] = [];
  const hashIndex = () => createHash('sha256').update(readFileSync(join(data, 'indexes', 'project-1.db'))).digest('hex');
  const cli = (...args: string[]) => runCli(args, env);
  const status = () => JSON.parse(cli('status', 'dynamic', '--json').stdout);
  const monitor = (project: string) => status().monitors?.find((m: any) => m.project === project);
  const agentOption = () => monitor('shop').agent_option as string;

  function runJvm(mode: string, portFile: string, agent?: string): ChildProcess {
    const args = [...(agent ? [agent] : []), '-cp', classes, 'com.example.shop.ShopApp', mode, portFile];
    const p = spawn('java', args, { stdio: ['ignore', 'ignore', 'pipe'] });
    jvms.push(p);
    return p;
  }

  async function finished(p: ChildProcess): Promise<number | null> {
    if (p.exitCode !== null) return p.exitCode;
    return new Promise((r) => p.once('exit', (code) => r(code)));
  }

  before(() => {
    data = tempDir('dynamic');
    classes = tempDir('classes');
    env = { PROJ_HOME: data, PROJ_AGENT_JAR: AGENT as string };
    const fixture = join(FIXTURES, 'dynamic-java');
    const c = spawnSync('javac', ['-nowarn', '-d', classes, ...javaSources(fixture)], { encoding: 'utf8' });
    assert.equal(c.status, 0, c.stderr);
    assert.ok([0, 3].includes(cli('add', 'shop', fixture).code ?? -1));
    assert.ok([0, 3].includes(cli('add', 'legacy', join(FIXTURES, 'legacy-java')).code ?? -1));
    indexHash = hashIndex();
  });

  after(() => {
    for (const p of jvms) if (p.exitCode === null) p.kill('SIGKILL');
    cli('stop', 'dynamic');
    cleanupTemp();
  });

  it('starts and stops monitors per project and for all, idempotently; the service exits when nothing is monitored', async () => {
    let r = cli('start', 'dynamic', 'shop');
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /Monitoring shop/);
    assert.match(r.stdout, /-javaagent:.*proj-agent\.jar=project=shop,service=.*,include=com\.example\.shop/);
    const service = JSON.parse(readFileSync(join(data, 'dynamic', 'service.json'), 'utf8'));
    assert.equal(statSync(join(data, 'dynamic', 'service.json')).mode & 0o777, 0o600);  // the token is private

    r = cli('start', 'dynamic', 'shop');
    assert.equal(r.code, 0);
    assert.match(r.stdout, /Already monitoring shop/);
    assert.equal(JSON.parse(readFileSync(join(data, 'dynamic', 'service.json'), 'utf8')).pid, service.pid);  // one service

    assert.equal(cli('start', 'dynamic', 'legacy').code, 0);
    assert.deepEqual(status().monitors.map((m: any) => [m.project, m.state]), [['legacy', 'IDLE'], ['shop', 'IDLE']]);

    r = cli('stop', 'dynamic', 'shop');
    assert.equal(r.code, 0);
    assert.match(r.stdout, /Stopped monitoring shop/);
    assert.match(r.stdout, /Still monitoring: legacy/);
    assert.deepEqual(status().monitors.map((m: any) => m.project), ['legacy']);
    r = cli('stop', 'dynamic', 'shop');
    assert.equal(r.code, 0);
    assert.match(r.stdout, /'shop' was not being monitored/);

    // Without a project: every project configured by an earlier start.
    r = cli('start', 'dynamic');
    assert.equal(r.code, 0, r.stderr);
    assert.deepEqual(status().monitors.map((m: any) => m.project), ['legacy', 'shop']);

    r = cli('stop', 'dynamic');
    assert.equal(r.code, 0);
    assert.match(r.stdout, /Stopped monitoring legacy, shop/);
    await waitFor('the service to exit', () => !existsSync(join(data, 'dynamic', 'service.json')));
    assert.match(cli('status', 'dynamic').stdout, /not running/);
    r = cli('stop', 'dynamic');
    assert.equal(r.code, 0);
    assert.match(r.stdout, /not running/);

    r = cli('start', 'dynamic', 'nosuch');
    assert.equal(r.code, 2);
    assert.match(r.stderr, /no project named 'nosuch'/);
    assert.equal(cli('start', 'everything').code, 2);
    assert.equal(cli('start', 'dynamic', 'shop', '--pid', 'abc').code, 2);
  });

  it('records a JVM from startup: request flows, overloads, recursion, concurrency, runtime-only calls', async () => {
    assert.equal(cli('start', 'dynamic', 'shop').code, 0);
    const p = runJvm('scenario', join(tempDir('port'), 'port'), agentOption());
    assert.equal(await finished(p), 0);
    await waitFor('the session to end', () => monitor('shop')?.history?.some((h: any) => h.state === 'FLUSHING' && /jvm_exit/.test(h.detail)));
    const states = monitor('shop').history.map((h: any) => h.state);
    for (const s of ['IDLE', 'TARGET_DISCOVERED', 'ATTACHED', 'CAPTURING', 'FLUSHING']) assert.ok(states.includes(s), `${s} in ${states}`);
    assert.equal(monitor('shop').state, 'IDLE');

    const traces = JSON.parse(cli('traces', 'shop', '--json', '--limit', '100').stdout).traces;
    const users = traces.filter((t: any) => t.http?.path?.startsWith('/users/'));
    assert.equal(users.length, 4);
    for (const t of users) {
      const tree = JSON.parse(cli('traces', 'shop', String(t.id), '--json').stdout);
      assert.equal(tree.startup_coverage, 'complete');
      const names = tree.nodes.map((n: any) => `${n.depth}:${n.method.class.split('.').pop()}.${n.method.name}`);
      // Concurrent requests stay apart: each tree is exactly one request.
      const formatDesc = tree.nodes.find((n: any) => n.method.name === 'format').method.descriptor;
      const id = t.http.path.split('/').pop();
      const expectBoolean = id === '1' || id === '3';  // odd ids use format(User, boolean)
      assert.equal(formatDesc.includes('Z)'), expectBoolean, `${t.http.path}: ${formatDesc}`);
      assert.deepEqual(names, [
        '0:BaseHandler.handle', '1:UserHandler.doHandle', '2:UserService.describe', '3:UserRepository.load', '4:User.<init>',
        '3:UserService.format', '3:UserService.depth', '4:UserService.depth', '5:UserService.depth', '6:UserService.depth',
        '1:BaseHandler.respond',
      ]);
      assert.equal(tree.nodes[1].evidence, 'STATIC_INFERRED');  // dispatch to the override
      assert.equal(tree.nodes[2].evidence, 'STATIC_CONFIRMED');
      assert.equal(tree.nodes[2].symbol.qualified_name, 'com.example.shop.service.UserService.describe');
    }
    // Startup: main() through Bootstrap.init, including the plugin started by reflection.
    const main = traces.find((t: any) => t.thread === 'main');
    const startup = JSON.parse(cli('traces', 'shop', String(main.id), '--json').stdout);
    const node = (name: string) => startup.nodes.find((n: any) => `${n.method.class.split('.').pop()}.${n.method.name}` === name);
    assert.equal(node('Config.load').evidence, 'STATIC_CONFIRMED');
    assert.equal(node('AuditPlugin.start').evidence, 'RUNTIME_OBSERVED');
    assert.equal(node('Stub.ping').evidence, 'RUNTIME_UNRESOLVED');
    assert.equal(node('Stub.ping').symbol, null);
    // The static view now carries runtime counts, without duplicate edges.
    const describeId = engine(data, 'search', { project: 'shop', query: 'UserService.describe' }).symbols[0].id;
    const fwd = engine(data, 'traverse', { project: 'shop', symbol_id: describeId, max_depth: 1 });
    const children = fwd.nodes.filter((n: any) => n.depth === 1);
    const keys = children.map((n: any) => `${n.symbol?.id ?? n.edge.target_text}|${n.edge.kind}`);
    assert.equal(new Set(keys).size, keys.length);
    assert.equal(children.filter((n: any) => n.symbol?.name === 'format').reduce((s: number, n: any) => s + (n.edge.observed ?? 0), 0), 4);

    // The target restarted: a second session of its own, nothing left open, the same traces.
    const again = runJvm('scenario', join(tempDir('port'), 'port'), agentOption());
    assert.equal(await finished(again), 0);
    await waitFor('the second session to end', () => {
      const s = engine(data, 'runtime_sessions', { project: 'shop' }).sessions;
      return s.length === 2 && s.every((x: any) => x.status === 'ended') && s;
    });
    const all = JSON.parse(cli('traces', 'shop', '--json', '--limit', '200').stdout).traces;
    const perSession = new Map<number, number>();
    for (const t of all) perSession.set(t.session, (perSession.get(t.session) ?? 0) + 1);
    assert.deepEqual([...perSession.values()], [traces.length, traces.length]);
  });

  it('classifies the exceptions it saw and keeps their stacks in thrown order', async () => {
    const list = JSON.parse(cli('errors', 'shop', '--json').stdout).errors;
    const byType = (type: string, state: string) => list.find((e: any) => e.exception === type && e.state === state);
    assert.ok(byType('java.lang.NumberFormatException', 'CAUGHT'));
    const sql = byType('java.sql.SQLException', 'WRAPPED');
    assert.ok(sql);
    assert.equal(sql.wrapped_by, 'java.lang.IllegalStateException');
    // Two scenario runs, each with a retried GET: one record, counted four times.
    assert.equal(sql.occurrences, 4);
    const escaped = byType('java.lang.IllegalStateException', 'ESCAPED');
    assert.equal(escaped.rethrown, true);
    assert.ok(byType('java.lang.IllegalStateException', 'UNCAUGHT'));  // the crasher thread
    assert.ok(byType('java.lang.IllegalArgumentException', 'UNKNOWN'));  // swallowed inside CompletableFuture

    const detail = JSON.parse(cli('errors', 'shop', String(sql.id), '--json').stdout);
    assert.equal(detail.frames[0].method, 'save');  // deepest (most recent) first
    assert.equal(detail.frames[0].symbol.qualified_name, 'com.example.shop.repo.PaymentRepository.save');
    assert.equal(detail.frames[1].method, 'create');
    assert.equal(detail.throw_site.line, detail.frames[0].line);
    assert.equal(detail.caught_at.method.name, 'create');
    const esc = JSON.parse(cli('errors', 'shop', String(escaped.id), '--json').stdout);
    assert.equal(esc.causes[0].exception, 'java.sql.SQLException');
    assert.deepEqual(esc.events.map((e: any) => e.kind), ['thrown', 'exit', 'caught', 'thrown', 'exit', 'finally', 'thrown', 'exit']);

    const text = cli('errors', 'shop', String(sql.id)).stdout;
    assert.match(text, /Stack, most recent call first:\n1\. PaymentRepository\.save\(\)\n {3}PaymentRepository\.java:\d+ {2}(→|->) /);
    assert.match(text, /State: WRAPPED/);
  });

  it('attaches to a running JVM: later calls are recorded, earlier startup is not; a killed JVM is detected', async () => {
    const portFile = join(tempDir('port'), 'port');
    const p = runJvm('wait', portFile);
    const port = Number(await waitFor('the server port', () => existsSync(portFile) && readFileSync(portFile, 'utf8')));
    const r = cli('start', 'dynamic', 'shop', '--pid', String(p.pid));
    assert.equal(r.code, 0, r.stderr);
    await waitFor('the attached session', () => monitor('shop')?.sessions?.some((s: any) => s.pid === p.pid), 40000);
    const session = monitor('shop').sessions.find((s: any) => s.pid === p.pid);
    assert.equal(session.mode, 'attach');
    assert.equal(session.startup_coverage, 'attached_late');
    assert.equal(await httpGet(port, '/users/7'), 200);
    const trace = await waitFor('the request trace', () =>
      JSON.parse(cli('traces', 'shop', '--json', '--limit', '200').stdout).traces.find((t: any) => t.http?.path === '/users/7'),
    );
    const tree = JSON.parse(cli('traces', 'shop', String(trace.id), '--json').stdout);
    assert.equal(tree.startup_coverage, 'attached_late');
    assert.equal(tree.pid, p.pid);
    // main() started before the attach: no startup trace in this session.
    const sessionTraces = JSON.parse(cli('traces', 'shop', '--json', '--limit', '200').stdout).traces.filter((t: any) => t.session === trace.session);
    assert.ok(!sessionTraces.some((t: any) => t.root.name === 'main' || t.root.name === 'init'));
    p.kill('SIGKILL');
    await waitFor('the lost connection', () => monitor('shop')?.history?.some((h: any) => h.state === 'FLUSHING' && new RegExp(`pid ${p.pid}`).test(h.detail)));
    const sessions = engine(data, 'runtime_sessions', { project: 'shop' }).sessions;
    assert.equal(sessions.find((s: any) => s.pid === p.pid).status, 'interrupted');
  });

  it('keeps what it recorded across service restarts and marks sessions a crashed service left open', async () => {
    const before = JSON.parse(cli('traces', 'shop', '--json').stdout).total;
    assert.equal(cli('stop', 'dynamic').code, 0);
    await waitFor('the service to exit', () => !existsSync(join(data, 'dynamic', 'service.json')));
    assert.equal(cli('start', 'dynamic', 'shop').code, 0);
    assert.equal(JSON.parse(cli('traces', 'shop', '--json').stdout).total, before);

    // A JVM connected when both it and the service die.
    const portFile = join(tempDir('port'), 'port');
    const p = runJvm('wait', portFile, agentOption());
    await waitFor('the connected JVM', () => monitor('shop')?.sessions?.some((s: any) => s.pid === p.pid));
    const service = JSON.parse(readFileSync(join(data, 'dynamic', 'service.json'), 'utf8'));
    process.kill(service.pid, 'SIGKILL');
    p.kill('SIGKILL');
    await finished(p);
    assert.equal(cli('start', 'dynamic', 'shop').code, 0);  // a new service; it resumes the monitor
    const sessions = engine(data, 'runtime_sessions', { project: 'shop' }).sessions;
    const crashed = sessions.find((s: any) => s.pid === p.pid);
    assert.equal(crashed.status, 'interrupted');
    assert.match(crashed.end_reason, /service stopped/);
    // The static index was only read: byte for byte what `proj add` wrote.
    assert.equal(hashIndex(), indexHash);
    // Observations live in their own stores (one per monitored project), next to the static indexes.
    assert.deepEqual(readdirSync(join(data, 'runtime')).filter((f) => f.endsWith('.db')).sort(), ['project-1.db', 'project-2.db']);
    assert.deepEqual(readdirSync(join(data, 'indexes')).filter((f) => f.endsWith('.db')).sort(), ['project-1.db', 'project-2.db']);
  });
});

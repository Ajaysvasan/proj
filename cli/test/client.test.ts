import assert from 'node:assert/strict';
import { after, describe, it } from 'node:test';

import { EngineProcessError, EngineSession, runEngine } from '../src/engine/client.js';
import { cleanupTemp, ENGINE, fakeEngine, tempDir } from './helpers.js';

after(cleanupTemp);

const ok = (body: string) => JSON.stringify({ protocol: 1, id: 1, op: 'info', status: 'ok', result: body, diagnostics: [], error: null });

describe('one-shot engine requests', () => {
  it('returns the real engine response', async () => {
    const res = await runEngine('info', {}, { enginePath: ENGINE, dataDir: tempDir('data') });
    assert.equal(res.status, 'ok');
    assert.equal(res.result.protocol_version, 1);
    assert.ok(res.result.languages.includes('python'));
  });

  it('reports an engine crash with its exit code and stderr', async () => {
    const engine = fakeEngine(tempDir('fake'), 'crash', 'cat >/dev/null; echo "fatal: oops" >&2; exit 3');
    await assert.rejects(runEngine('info', {}, { enginePath: engine, dataDir: tempDir('data') }), (err: EngineProcessError) => {
      assert.equal(err.kind, 'crashed');
      assert.equal(err.exitCode, 3);
      assert.match(err.stderr, /fatal: oops/);
      return true;
    });
  });

  it('rejects output that is not JSON', async () => {
    const engine = fakeEngine(tempDir('fake'), 'garbage', 'cat >/dev/null; echo "hello, I am not JSON"');
    await assert.rejects(runEngine('info', {}, { enginePath: engine, dataDir: tempDir('data') }), (err: EngineProcessError) => err.kind === 'malformed_response');
  });

  it('rejects JSON that is not a protocol envelope', async () => {
    const engine = fakeEngine(tempDir('fake'), 'shape', `cat >/dev/null; echo '{"protocol":1,"status":"maybe"}'`);
    await assert.rejects(runEngine('info', {}, { enginePath: engine, dataDir: tempDir('data') }), /invalid response: unknown status/);
  });

  it('rejects an empty response', async () => {
    const engine = fakeEngine(tempDir('fake'), 'silent', 'cat >/dev/null; exit 0');
    await assert.rejects(runEngine('info', {}, { enginePath: engine, dataDir: tempDir('data') }), /without a response/);
  });

  it('times out and terminates a hung engine', async () => {
    const engine = fakeEngine(tempDir('fake'), 'hang', 'exec sleep 30');
    const started = Date.now();
    await assert.rejects(runEngine('info', {}, { enginePath: engine, dataDir: tempDir('data'), timeoutMs: 300 }), (err: EngineProcessError) => err.kind === 'timeout');
    assert.ok(Date.now() - started < 5000);
  });

  it('reports a missing engine binary', async () => {
    await assert.rejects(runEngine('info', {}, { enginePath: '/nonexistent/proj-engine', dataDir: tempDir('data') }), (err: EngineProcessError) => err.kind === 'spawn_failed');
  });

  it('passes arguments as data, never through a shell', async () => {
    // The fake engine echoes its stdin back inside a valid envelope.
    const dir = tempDir('fake');
    const engine = fakeEngine(dir, 'echo', `input=$(cat); printf '{"protocol":1,"id":1,"op":"x","status":"ok","result":%s,"diagnostics":[],"error":null}\\n' "$input"`);
    const nasty = '/tmp/$(touch pwned); `rm -rf /`; "quoted" path';
    const res = await runEngine('add_project', { path: nasty }, { enginePath: engine, dataDir: dir });
    assert.equal(res.result.args.path, nasty);
    void ok;
  });
});

describe('engine session (--serve)', () => {
  it('answers several requests over one process and closes cleanly', async () => {
    const session = new EngineSession({ enginePath: ENGINE, dataDir: tempDir('data') });
    after(() => session.close());
    const [a, b] = await Promise.all([session.request('info', {}), session.request('list_projects', {})]);
    assert.equal(a.status, 'ok');
    assert.equal(b.status, 'ok');
    assert.ok(session.running);
    const bad = await session.request('no_such_op', {});
    assert.equal(bad.status, 'unsupported_operation');
    await session.close();
    assert.equal(session.running, false);
    await assert.rejects(session.request('info', {}), /closed/);
  });

  it('fails in-flight requests when the engine dies, then restarts', async () => {
    const dir = tempDir('fake');
    // First process dies immediately; the marker file makes later ones use the real engine.
    const engine = fakeEngine(dir, 'flaky', `if [ ! -f "${dir}/started" ]; then touch "${dir}/started"; exit 9; fi\nexec "${ENGINE}" "$@"`);
    const session = new EngineSession({ enginePath: engine, dataDir: tempDir('data') });
    try {
      await assert.rejects(session.request('info', {}), (err: EngineProcessError) => err.kind === 'crashed');
      const res = await session.request('info', {});
      assert.equal(res.status, 'ok');
    } finally {
      await session.close();
    }
  });

  it('times out a request without blocking later ones', async () => {
    const dir = tempDir('fake');
    const engine = fakeEngine(dir, 'slow', `if [ ! -f "${dir}/started" ]; then touch "${dir}/started"; exec sleep 30; fi\nexec "${ENGINE}" "$@"`);
    const session = new EngineSession({ enginePath: engine, dataDir: tempDir('data'), timeoutMs: 2000 });
    try {
      await assert.rejects(session.request('info', {}), (err: EngineProcessError) => err.kind === 'timeout');
      // The hung process is being terminated while the next request already runs on a new one.
      const res = await session.request('info', {});
      assert.equal(res.status, 'ok');
    } finally {
      await session.close();
    }
  });
});

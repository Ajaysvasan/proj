import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, describe, it } from 'node:test';

import { appendAppLog, logFile, readAppLog } from '../src/applog.js';
import { exitCodeForAnalysis, parseSize } from '../src/commands/add.js';
import { exitCodeForStatus } from '../src/context.js';
import { EngineNotFoundError, locateEngine } from '../src/engine/locate.js';
import { validateEnvelope } from '../src/engine/protocol.js';
import { ExitCode } from '../src/exit-codes.js';
import { resolveDataDir } from '../src/paths.js';
import { describeRow, layoutTree, type TraverseNode } from '../src/ui/tree.js';
import { glyph } from '../src/format.js';
import { errorReport, traceLines } from '../src/ui/runtime_text.js';
import { cleanupTemp, tempDir } from './helpers.js';

after(cleanupTemp);

describe('data directory resolution', () => {
  it('prefers the option, then PROJ_HOME, then the platform default', () => {
    assert.equal(resolveDataDir('/x/opt', { PROJ_HOME: '/x/home' }), '/x/opt');
    assert.equal(resolveDataDir(undefined, { PROJ_HOME: '/x/home' }), '/x/home');
    assert.equal(resolveDataDir(undefined, { HOME: '/h' }, 'linux'), '/h/.local/share/proj');
    assert.equal(resolveDataDir(undefined, { HOME: '/h', XDG_DATA_HOME: '/xdg' }, 'linux'), '/xdg/proj');
    assert.equal(resolveDataDir(undefined, { HOME: '/h', XDG_DATA_HOME: 'relative' }, 'linux'), '/h/.local/share/proj');
    assert.equal(resolveDataDir(undefined, { HOME: '/Users/a' }, 'darwin'), '/Users/a/Library/Application Support/proj');
  });
});

describe('exit codes', () => {
  it('maps analysis outcomes and engine statuses', () => {
    assert.equal(exitCodeForAnalysis('success'), ExitCode.Success);
    assert.equal(exitCodeForAnalysis('partial'), ExitCode.Partial);
    assert.equal(exitCodeForAnalysis('no_supported_files'), ExitCode.NoSupportedFiles);
    assert.equal(exitCodeForStatus('invalid_arguments'), ExitCode.UserError);
    assert.equal(exitCodeForStatus('not_found'), ExitCode.UserError);
    assert.equal(exitCodeForStatus('conflict'), ExitCode.UserError);
    assert.equal(exitCodeForStatus('index_error'), ExitCode.InternalError);
    assert.equal(exitCodeForStatus('internal_error'), ExitCode.InternalError);
  });

  it('parses size options', () => {
    assert.equal(parseSize('512'), 512);
    assert.equal(parseSize('2k'), 2048);
    assert.equal(parseSize('1MiB'), 1024 * 1024);
    assert.throws(() => parseSize('lots'));
  });
});

describe('protocol envelope validation', () => {
  it('accepts a well-formed envelope and rejects others', () => {
    const ok = { protocol: 1, id: 1, op: 'info', status: 'ok', result: {}, diagnostics: [], error: null };
    assert.equal(validateEnvelope(ok), null);
    assert.match(validateEnvelope([]) ?? '', /not a JSON object/);
    assert.match(validateEnvelope({ ...ok, protocol: 2 }) ?? '', /protocol version/);
    assert.match(validateEnvelope({ ...ok, status: 'great' }) ?? '', /unknown status/);
    assert.match(validateEnvelope({ protocol: 1, status: 'ok' }) ?? '', /incomplete/);
  });
});

describe('engine location', () => {
  it('reports an explicit path that does not exist', () => {
    assert.throws(() => locateEngine('/definitely/not/here/proj-engine'), EngineNotFoundError);
    assert.throws(() => locateEngine(undefined, { PROJ_ENGINE: '/nope/engine', PATH: '' }), /PROJ_ENGINE/);
  });
});

describe('application log', () => {
  it('appends JSON lines, filters by level and rotates at the size limit', () => {
    const dir = tempDir('applog');
    appendAppLog(dir, 'INFO', 'test', 'hello');
    appendAppLog(dir, 'ERROR', 'test', 'boom', { op: 'x' });
    const errors = readAppLog(dir, 10, 'WARN');
    assert.equal(errors.length, 1);
    assert.equal(errors[0]!.message, 'boom');
    assert.equal(errors[0]!.source, 'cli');
    assert.equal(readAppLog(dir, 10, 'INFO').length, 2);
    // Grow the file past 1 MiB: the next append rotates it.
    writeFileSync(logFile(dir), 'x'.repeat(1024 * 1024 + 10));
    appendAppLog(dir, 'WARN', 'test', 'after rotation');
    assert.ok(readFileSync(logFile(dir) + '.1', 'utf8').length > 1024 * 1024);
    assert.equal(readFileSync(logFile(dir), 'utf8').trim().split('\n').length, 1);
    // A torn/foreign line in the log is skipped rather than crashing.
    writeFileSync(join(dir, 'logs', 'proj.log'), 'not json\n' + JSON.stringify({ ts: 't', level: 'ERROR', source: 'engine', message: 'ok' }) + '\n');
    assert.equal(readAppLog(dir, 10).length, 1);
  });
});

describe('flow tree layout', () => {
  const sym = (id: number, name: string) => ({ id, name, qualified_name: name, kind: 'function', path: `${name}.js`, line: id });
  const nodes: TraverseNode[] = [
    { index: 0, parent: null, depth: 0, symbol: sym(1, 'a'), edge: null },
    { index: 1, parent: 0, depth: 1, symbol: sym(2, 'b'), edge: { kind: 'calls', status: 'resolved', line: 3, path: 'a.js', sites: 2 } },
    { index: 2, parent: 0, depth: 1, symbol: null, edge: { kind: 'calls', status: 'unresolved', line: 4, path: 'a.js', target_text: 'x.y', note: 'dynamic' } },
    { index: 3, parent: 1, depth: 2, symbol: sym(1, 'a'), edge: { kind: 'calls', status: 'resolved', line: 9, path: 'b.js' }, seen: true },
  ];
  it('orders rows depth-first and marks cycles', () => {
    const rows = layoutTree(nodes);
    assert.deepEqual(rows.map((r) => r.node.index), [0, 1, 3, 2]);
    const text = rows.map((r) => r.prefix + describeRow(r));
    assert.match(text[1]!, /b\s+b\.js:2 x2$/);
    assert.match(text[2]!, /a\s+a\.js:1 .*shown above/);
    assert.match(text[3]!, /x\.y \[unresolved: dynamic\]/);
  });
  it('handles an empty result', () => {
    assert.deepEqual(layoutTree([]), []);
  });
});

describe('runtime tracing text', () => {
  const sym = (name: string, path: string, line: number) => ({ id: line, name, qualified_name: `a.${name}`, kind: 'method', path, line });

  it('marks calls that ran, calls only the runtime saw, and code outside the index', () => {
    const row = (edge: any, symbol: any = null) => describeRow({ prefix: '', node: { index: 1, parent: 0, depth: 1, symbol, edge } as TraverseNode });
    const base = { kind: 'calls', line: 3, path: 'A.java' };
    assert.equal(row({ ...base, status: 'resolved', evidence: 'STATIC_CONFIRMED', observed: 3 }, sym('load', 'Repo.java', 7)), `a.load  Repo.java:7 [ran 3${glyph.times}]`);
    assert.equal(row({ ...base, status: 'resolved' }, sym('load', 'Repo.java', 7)), 'a.load  Repo.java:7');  // nothing traced: as before
    assert.equal(row({ ...base, status: 'observed', evidence: 'RUNTIME_OBSERVED', observed: 1 }, sym('start', 'P.java', 8)), `a.start  P.java:8 [runtime only, ran 1${glyph.times}]`);
    assert.equal(
      row({ ...base, status: 'observed', evidence: 'RUNTIME_UNRESOLVED', observed: 2, target_text: 'gen.Stub.ping()V' }),
      `gen.Stub.ping()V [runtime only, not in the index, ran 2${glyph.times}]`,
    );
    assert.equal(
      row({ ...base, status: 'resolved', evidence: 'STATIC_CONFIRMED', observed: 2, observed_target: sym('UserHandler.doHandle', 'U.java', 9) }, sym('doHandle', 'B.java', 20)),
      `a.doHandle  B.java:20 [ran 2${glyph.times} as a.UserHandler.doHandle]`,
    );
  });

  it('prints an error with the most recent frame first and each location under it', () => {
    const lines = errorReport({
      exception: 'java.sql.SQLException', message: 'empty item', state: 'WRAPPED', rethrown: false, wrapped_by: 'java.lang.IllegalStateException',
      occurrences: 2, first_seen: 't1', last_seen: 't2', thread: 'worker',
      throw_site: { method: { display: 'PaymentRepository.save(String)' }, line: 8, symbol: sym('save', 'src/PaymentRepository.java', 6) },
      caught_at: { method: { display: 'OrderService.create(String)' }, line: 13, symbol: null }, caught_type: 'java.sql.SQLException',
      top_app_frame: { class: 'a.PaymentRepository', method: 'save', line: 8, symbol: null },
      frames: [
        { class: 'a.PaymentRepository', method: 'save', file: 'PaymentRepository.java', line: 8, application: true, symbol: sym('save', 'src/PaymentRepository.java', 6) },
        { class: 'java.lang.Thread', method: 'run', file: 'Thread.java', line: 1583, application: false, symbol: null },
      ],
      frames_omitted: 0, causes: [], suppressed: 0, events: [], traces: [],
    });
    const stack = lines.indexOf('Stack, most recent call first:');
    assert.deepEqual(lines.slice(stack + 1, stack + 5), [
      '1. PaymentRepository.save()',
      `   PaymentRepository.java:8  ${glyph.arrow} src/PaymentRepository.java`,
      '2. Thread.run()  [outside the project]',
      '   Thread.java:1583',
    ]);
    assert.ok(lines.includes(`Thrown at: PaymentRepository.save(String) line 8  ${glyph.arrow} src/PaymentRepository.java:8`));
    assert.ok(lines.some((l) => l.startsWith('State: WRAPPED, caught and wrapped in another exception; wrapped by java.lang.IllegalStateException')));
    // Thrown in a library: said so, not guessed.
    assert.ok(errorReport({ exception: 'X', state: 'CAUGHT', occurrences: 1, frames: [], frames_omitted: 0 }).some((l) => l.startsWith('Thrown at: outside application code')));
  });

  it('indents a trace by depth and labels evidence', () => {
    const lines = traceLines({
      thread: 'main', pid: 1, duration_us: 1500, status: 'complete', startup_coverage: 'attached_late', dropped_nodes: 0,
      nodes: [
        { depth: 0, method: { display: 'Boot.init()' }, calls: 1, exits_by_exception: 0, symbol: sym('init', 'Boot.java', 3) },
        { depth: 1, method: { display: 'Plugin.start()' }, calls: 2, exits_by_exception: 0, evidence: 'RUNTIME_OBSERVED', symbol: sym('start', 'P.java', 8) },
        { depth: 2, method: { display: 'Stub.ping()' }, calls: 1, exits_by_exception: 1, evidence: 'RUNTIME_UNRESOLVED', symbol: null },
      ],
      errors: [],
    });
    assert.match(lines[0]!.text, /agent attached to a running JVM/);
    assert.equal(lines[2]!.text, `  Plugin.start() ${glyph.times}2 [runtime only]  ${glyph.arrow} P.java:8`);
    assert.equal(lines[3]!.text, '    Stub.ping() [exception] [not in the index]');
    assert.equal(lines[3]!.symbol, undefined);
  });
});

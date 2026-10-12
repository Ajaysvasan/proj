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

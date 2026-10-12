import assert from 'node:assert/strict';
import { chmodSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';

import { cleanupTemp, copyFixture, fakeEngine, runCli, spawnCli, tempDir } from './helpers.js';

after(cleanupTemp);

describe('proj add', () => {
  let home: string;
  let project: string;
  before(() => {
    home = tempDir('home');
    // A path with spaces and non-ASCII characters.
    const parent = join(tempDir('src'), 'my projects ä');
    mkdirSync(parent);
    project = join(parent, 'service');
    mkdirSync(join(project, 'lib'), { recursive: true });
    writeFileSync(join(project, 'main.py'), 'from lib.util import helper\n\n\ndef main():\n    helper()\n\n\nif __name__ == "__main__":\n    main()\n');
    writeFileSync(join(project, 'lib', '__init__.py'), '');
    writeFileSync(join(project, 'lib', 'util.py'), 'def helper():\n    return 1\n');
  });

  it('analyzes a valid project in a path with spaces (exit 0)', () => {
    const r = runCli(['add', 'svc', project], { PROJ_HOME: home });
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /Analysis complete/);
    assert.match(r.stdout, /my projects ä\/service/);
    assert.match(r.stdout, /Symbols\s+2/);
    assert.match(r.stdout, /Index\s+.*indexes\/project-1\.db/);
  });

  it('rejects a duplicate name and a duplicate path (exit 2)', () => {
    const dupName = runCli(['add', 'svc', project], { PROJ_HOME: home });
    assert.equal(dupName.code, 2);
    assert.match(dupName.stderr, /already registered/);
    const dupPath = runCli(['add', 'other', project], { PROJ_HOME: home });
    assert.equal(dupPath.code, 2);
    assert.match(dupPath.stderr, /already registered as project 'svc'/);
  });

  it('re-analyzes with --reanalyze and emits JSON on request', () => {
    const r = runCli(['add', 'svc', project, '--reanalyze', '--json'], { PROJ_HOME: home });
    assert.equal(r.code, 0, r.stderr);
    const summary = JSON.parse(r.stdout);
    assert.equal(summary.status, 'success');
    assert.equal(summary.counts.symbols, 2);
    assert.equal(summary.run_id, 2);
  });

  it('rejects a nonexistent directory and an invalid name (exit 2)', () => {
    const missing = runCli(['add', 'ghost', join(project, 'nope')], { PROJ_HOME: home });
    assert.equal(missing.code, 2);
    assert.match(missing.stderr, /does not exist/);
    const badName = runCli(['add', '../evil', project], { PROJ_HOME: home });
    assert.equal(badName.code, 2);
    assert.match(badName.stderr, /invalid project name/);
  });

  it('reports partial analysis (exit 3) and no supported files (exit 4)', () => {
    const fixtureHome = tempDir('home');
    const express = runCli(['add', 'express', copyFixture('express-api', tempDir('fx'))], { PROJ_HOME: fixtureHome });
    assert.equal(express.code, 3, express.stderr);
    assert.match(express.stdout, /partial/);
    assert.match(express.stdout, /syntax_error src\/broken\.js:1/);
    const docs = join(tempDir('docs'), 'docs');
    mkdirSync(docs);
    writeFileSync(join(docs, 'README.md'), '# docs only\n');
    const empty = runCli(['add', 'docs', docs], { PROJ_HOME: fixtureHome });
    assert.equal(empty.code, 4);
    assert.match(empty.stdout, /No supported source files found/);
  });

  it('quiet mode prints nothing on success', () => {
    const r = runCli(['--quiet', 'add', 'svc', project, '--reanalyze'], { PROJ_HOME: home });
    assert.equal(r.code, 0);
    assert.equal(r.stdout, '');
  });
});

describe('proj show', () => {
  it('prints a summary for an indexed project when not attached to a terminal', () => {
    const home = tempDir('home');
    assert.equal(runCli(['add', 'cpp', copyFixture('cpp-app', tempDir('fx'))], { PROJ_HOME: home }).code, 0);
    const r = runCli(['show', 'cpp'], { PROJ_HOME: home });
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /proj · cpp \[success\]/);
    assert.match(r.stdout, /main_function\s+src\/main\.cpp:6/);
    assert.match(r.stdout, /needs a terminal/);
    const check = runCli(['show', 'cpp', '--check'], { PROJ_HOME: home });
    assert.equal(check.code, 0, check.stderr);
    assert.match(check.stdout, /Index integrity for cpp \(schema v2\): ok/);
    assert.match(check.stdout, /edges_with_missing_target/);
  });

  it('rejects an unknown project (exit 2)', () => {
    const r = runCli(['show', 'nobody'], { PROJ_HOME: tempDir('home') });
    assert.equal(r.code, 2);
    assert.match(r.stderr, /no project named 'nobody'/);
  });

  it('explains a missing or corrupt index', () => {
    const home = tempDir('home');
    const src = join(tempDir('src'), 'p');
    mkdirSync(src);
    writeFileSync(join(src, 'a.js'), 'function a() {}\n');
    assert.equal(runCli(['add', 'p', src], { PROJ_HOME: home }).code, 0);
    const index = join(home, 'indexes', 'project-1.db');
    writeFileSync(index, 'corrupted bytes, not sqlite');
    const corrupt = runCli(['show', 'p'], { PROJ_HOME: home });
    assert.equal(corrupt.code, 1);
    assert.match(corrupt.stderr, /cannot be opened.*--reanalyze/s);
  });
});

describe('proj refresh', () => {
  it('re-indexes only what changed and keeps flows correct', () => {
    const home = tempDir('home');
    const src = join(tempDir('src'), 'svc');
    mkdirSync(src);
    writeFileSync(join(src, 'util.js'), 'function format(x) {\n  return String(x);\n}\n\nmodule.exports = { format };\n');
    writeFileSync(join(src, 'app.js'), "const { format } = require('./util');\n\nfunction main() {\n  return format(1);\n}\n\nmain();\n");
    assert.equal(runCli(['add', 'svc', src], { PROJ_HOME: home }).code, 0);

    const unchanged = runCli(['refresh', 'svc', '--json'], { PROJ_HOME: home });
    assert.equal(unchanged.code, 0, unchanged.stderr);
    const u = JSON.parse(unchanged.stdout);
    assert.equal(u.mode, 'refresh');
    assert.equal(u.stats.refresh.unchanged_files, 2);
    assert.equal(u.stats.refresh.changed_files, 0);

    // Rename the function, add a same-named function in a new file, remove nothing.
    writeFileSync(join(src, 'util.js'), 'function formatValue(x) {\n  return String(x);\n}\n\nmodule.exports = { formatValue };\n');
    writeFileSync(join(src, 'other.js'), 'function format(x) {\n  return x;\n}\n\nmodule.exports = { format };\n');
    const r = runCli(['refresh', 'svc'], { PROJ_HOME: home });
    assert.equal(r.code, 0, r.stderr);
    assert.match(r.stdout, /Refresh complete/);
    assert.match(r.stdout, /Changes\s+1 changed, 1 added, 0 removed, 1 unchanged/);
    // main() still imports `format` from ./util, which no longer exports it: the
    // call must become unresolved, not jump to the unrelated other.js format.
    const show = runCli(['show', 'svc', '--check'], { PROJ_HOME: home });
    assert.equal(show.code, 0, show.stdout);
    const log = runCli(['log', 'svc', '--json'], { PROJ_HOME: home });
    const runs = JSON.parse(log.stdout).runs;
    assert.equal(runs.length, 3);
    assert.equal(runs[0].stats.mode, 'refresh');

    rmSync(join(src, 'other.js'));
    const removed = runCli(['refresh', 'svc', '--json'], { PROJ_HOME: home });
    assert.equal(JSON.parse(removed.stdout).stats.refresh.removed_files, 1);
  });

  it('reports an unknown project and a missing index', () => {
    const home = tempDir('home');
    const unknown = runCli(['refresh', 'ghost'], { PROJ_HOME: home });
    assert.equal(unknown.code, 2);
    assert.match(unknown.stderr, /no project named 'ghost'/);
    const src = join(tempDir('src'), 'p');
    mkdirSync(src);
    writeFileSync(join(src, 'a.py'), 'def a():\n    pass\n');
    assert.equal(runCli(['add', 'p', src], { PROJ_HOME: home }).code, 0);
    rmSync(join(home, 'indexes', 'project-1.db'));
    const missing = runCli(['refresh', 'p'], { PROJ_HOME: home });
    assert.equal(missing.code, 2);
    assert.match(missing.stderr, /has no index to refresh/);
  });
});

describe('output to a closed pipe', () => {
  it('keeps the exit code and prints no error when the reader exits early (proj add | head)', async () => {
    const home = tempDir('home');
    const src = join(tempDir('src'), 'p');
    mkdirSync(src);
    writeFileSync(join(src, 'a.py'), 'def a():\n    return 1\n');
    const child = spawnCli(['add', 'p', src], { PROJ_HOME: home });
    child.stdout!.destroy(); // the reader is gone before the summary is written
    let stderr = '';
    child.stderr!.on('data', (d: Buffer) => (stderr += d.toString()));
    const code = await new Promise<number | null>((done) => child.on('close', (c) => done(c)));
    assert.equal(code, 0, stderr);
    assert.doesNotMatch(stderr, /EPIPE|internal error/);
    // The analysis itself completed.
    assert.equal(runCli(['show', 'p', '--check'], { PROJ_HOME: home }).code, 0);
  });
});

describe('proj log', () => {
  it('lists successful and failed analyses and run details', () => {
    const home = tempDir('home');
    const src = join(tempDir('src'), 'p');
    mkdirSync(src);
    writeFileSync(join(src, 'a.js'), 'function a() { b(); }\nfunction b() {}\n');
    assert.equal(runCli(['add', 'p', src], { PROJ_HOME: home }).code, 0);
    // Make the next analysis fail: the index directory becomes read-only.
    const indexes = join(home, 'indexes');
    chmodSync(indexes, 0o555);
    let failed;
    try {
      failed = runCli(['add', 'p', src, '--reanalyze'], { PROJ_HOME: home });
    } finally {
      chmodSync(indexes, 0o755);
    }
    if (process.getuid && process.getuid() === 0) return; // root ignores permissions
    assert.equal(failed.code, 1);
    assert.match(failed.stderr, /analysis failed/);
    assert.match(failed.stderr, /previous index was kept/);

    const log = runCli(['log'], { PROJ_HOME: home });
    assert.equal(log.code, 0);
    assert.match(log.stdout, /^RUN\s+PROJECT/m);
    assert.match(log.stdout, /^2\s+p\s+.*failed/m);
    assert.match(log.stdout, /^1\s+p\s+.*success/m);
    assert.match(log.stdout, /run 2: cannot open database/);

    const detail = runCli(['log', 'p', '--run', '2'], { PROJ_HOME: home });
    assert.equal(detail.code, 0);
    assert.match(detail.stdout, /Internal \(application\) problems:/);
    assert.match(detail.stdout, /\[internal\/analysis_failed\]/);

    const json = JSON.parse(runCli(['log', '--json'], { PROJ_HOME: home }).stdout);
    assert.equal(json.runs.length, 2);
    // The old index still works.
    assert.equal(runCli(['show', 'p'], { PROJ_HOME: home }).code, 0);
  });

  it('reports an unknown project and an empty history', () => {
    const home = tempDir('home');
    assert.equal(runCli(['log', 'nobody'], { PROJ_HOME: home }).code, 2);
    const empty = runCli(['log'], { PROJ_HOME: home });
    assert.equal(empty.code, 0);
    assert.match(empty.stdout, /No analysis runs recorded yet/);
  });
});

describe('engine failures as seen by the CLI', () => {
  it('turns an engine crash into exit 1 and records an application error', () => {
    const home = tempDir('home');
    const engine = fakeEngine(tempDir('fake'), 'crash', 'cat >/dev/null; echo "Segmentation fault" >&2; kill -SEGV $$');
    const r = runCli(['show', 'anything'], { PROJ_HOME: home, PROJ_ENGINE: engine });
    assert.equal(r.code, 1);
    assert.match(r.stderr, /killed by SIGSEGV/);
    const log = readFileSync(join(home, 'logs', 'proj.log'), 'utf8');
    assert.match(log, /"kind":"crashed"/);
    // `proj log` (with the real engine) surfaces it as an application error.
    const shown = runCli(['log'], { PROJ_HOME: home });
    assert.match(shown.stdout, /application warnings and errors/);
    assert.match(shown.stdout, /killed by SIGSEGV/);
  });

  it('turns a malformed engine response into exit 1', () => {
    const engine = fakeEngine(tempDir('fake'), 'garbage', 'cat >/dev/null; echo "<html>not json</html>"');
    const r = runCli(['log'], { PROJ_HOME: tempDir('home'), PROJ_ENGINE: engine });
    assert.equal(r.code, 1);
    assert.match(r.stderr, /not valid JSON/);
  });

  it('explains a missing engine binary', () => {
    const r = runCli(['log'], { PROJ_HOME: tempDir('home'), PROJ_ENGINE: '/no/such/proj-engine' });
    assert.equal(r.code, 1);
    assert.match(r.stderr, /proj-engine not found/);
  });
});

describe('command-line parsing', () => {
  it('handles help, version, unknown commands and missing arguments', () => {
    const help = runCli(['--help']);
    assert.equal(help.code, 0);
    assert.match(help.stdout, /add \[options\] <name> <path>/);
    assert.equal(runCli(['--version']).stdout.trim(), '0.1.0');
    const unknown = runCli(['frobnicate']);
    assert.equal(unknown.code, 2);
    assert.match(unknown.stderr, /unknown command/);
    const missing = runCli(['add', 'only-a-name']);
    assert.equal(missing.code, 2);
    assert.match(missing.stderr, /missing required argument/);
  });
});

// Drives the interactive UI with simulated keystrokes against a real engine
// session and a real index of the Express fixture.
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { after, before, describe, it } from 'node:test';

import { render } from 'ink';
import React from 'react';

import { EngineSession, runEngine } from '../src/engine/client.js';
import { App } from '../src/ui/App.js';
import { cleanupTemp, copyFixture, ENGINE, tempDir } from './helpers.js';

class FakeStdout extends EventEmitter {
  columns = 140;
  rows = 40;
  isTTY = true;
  frames: string[] = [];
  write = (frame: string) => {
    this.frames.push(frame);
    return true;
  };
  lastFrame = () => this.frames[this.frames.length - 1] ?? '';
}

class FakeStdin extends EventEmitter {
  isTTY = true;
  private data: string | null = null;
  send(data: string) {
    this.data = data;
    this.emit('readable');
    this.emit('data', data);
  }
  read = () => {
    const d = this.data;
    this.data = null;
    return d;
  };
  setEncoding() {}
  setRawMode() {}
  resume() {}
  pause() {}
  ref() {}
  unref() {}
}

const KEY = { enter: '\r', escape: '\u001b', tab: '\t', down: '\u001b[B' };

async function waitFor(stdout: FakeStdout, test: (frame: string) => boolean, what: string, timeoutMs = 10000): Promise<string> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const frame = stdout.lastFrame();
    if (test(frame)) return frame;
    await new Promise((r) => setTimeout(r, 20));
  }
  throw new Error(`timed out waiting for ${what}; last frame:\n${stdout.lastFrame()}`);
}

describe('interactive UI', () => {
  let dataDir: string;
  let status: any;
  before(async () => {
    dataDir = tempDir('ui');
    const root = copyFixture('express-api', tempDir('fx'));
    const added = await runEngine('add_project', { name: 'express', path: root }, { enginePath: ENGINE, dataDir });
    assert.equal(added.status, 'partial');
    status = (await runEngine('project_status', { project: 'express' }, { enginePath: ENGINE, dataDir })).result;
  });
  after(cleanupTemp);

  it('searches, opens symbols and routes, navigates back, and exits cleanly', async () => {
    const stdout = new FakeStdout();
    const stdin = new FakeStdin();
    const session = new EngineSession({ enginePath: ENGINE, dataDir, timeoutMs: 15000 });
    const app = render(<App session={session} project="express" summary={status.index} rootPath={status.project.root_path} />, {
      stdout: stdout as any,
      stdin: stdin as any,
      debug: true,
      exitOnCtrlC: false,
      patchConsole: false,
    });
    // Ink creates this promise lazily; request it before anything can exit (as show.ts does).
    const exitPromise = app.waitUntilExit();
    try {
      let frame = await waitFor(stdout, (f) => f.includes('proj · express') && f.includes('press / to search'), 'home screen');
      assert.match(frame, /\[partial\]/);
      assert.match(frame, /routes 7/);

      stdin.send('/');
      await waitFor(stdout, (f) => f.includes('type to search'), 'search focus');
      stdin.send('q'); // typed into the search box, does not quit
      stdin.send('\u007f');
      stdin.send('getUser');
      frame = await waitFor(stdout, (f) => f.includes('src/controllers/userController.js:4') && f.includes('src/legacy.js:4'), 'duplicate getUser results');

      stdin.send(KEY.enter);
      frame = await waitFor(stdout, (f) => f.includes('function getUser') && f.includes('UserService.findById'), 'symbol view with callees');
      assert.match(frame, /Callees/);
      assert.match(frame, /UserRepository\.findById|formatUser/);
      assert.match(frame, /res\.json \[unresolved/);

      stdin.send(KEY.tab);
      frame = await waitFor(stdout, (f) => f.includes('route src/routes/users.js'), 'callers tab (route registration)');

      stdin.send(KEY.tab);
      frame = await waitFor(stdout, (f) => f.includes('Recognized handlers in this symbol (1)'), 'exceptions tab');
      assert.match(frame, /NotFoundError in UserService\.findById/);

      stdin.send(KEY.escape);
      frame = await waitFor(stdout, (f) => f.includes('src/legacy.js:4'), 'back to preserved search results');

      stdin.send('/');
      await waitFor(stdout, (f) => f.includes('type to search'), 'search focus again');
      stdin.send('\u0015'); // Ctrl+U clears the search box
      await waitFor(stdout, (f) => /^Search:\s*$/m.test(f), 'cleared search');
      stdin.send('GET /api/users/42');
      frame = await waitFor(stdout, (f) => f.includes('route   GET /api/users/:id'), 'route search by concrete URL');
      stdin.send(KEY.enter);
      frame = await waitFor(stdout, (f) => f.includes('mounted     /api/users') && f.includes('handler     getUser'), 'route view');
      assert.match(frame, /middleware  mount:express\.json\(\) \[unresolved\] → mount:requestLogger → route:authenticate/);
      assert.match(frame, /startup     src\/server\.js → src\/app\.js → src\/routes\/users\.js/);
      await waitFor(stdout, (f) => f.includes('UserService.findById'), 'route flow tree');

      stdin.send('?');
      await waitFor(stdout, (f) => f.includes('Keyboard shortcuts'), 'help overlay');
      stdin.send(KEY.escape);
      await waitFor(stdout, (f) => !f.includes('Keyboard shortcuts') && f.includes('handler     getUser'), 'help closed');

      stdin.send('h');
      await waitFor(stdout, (f) => f.includes('Search: GET /api/users/42'), 'home');
      stdin.send(KEY.escape); // leaves the search box
      stdin.send('e');
      frame = await waitFor(stdout, (f) => f.includes('package_main') && f.includes('src/server.js:1'), 'entry points');

      stdin.send('q');
      const exited = await Promise.race([
        exitPromise.then(() => true),
        new Promise<boolean>((r) => setTimeout(() => r(false), 5000)),
      ]);
      assert.equal(exited, true, 'app exits on q');
    } finally {
      app.unmount();
      await session.close();
    }
    assert.equal(session.running, false, 'engine process is closed after exit');
  });
});

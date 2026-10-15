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

// Ink attaches a view's input handler in an effect that runs after the frame is drawn, so a key sent
// as soon as a new view appears can arrive before anything listens. Send it again until the UI reacts.
async function press(stdin: FakeStdin, stdout: FakeStdout, key: string, test: (frame: string) => boolean, what: string): Promise<string> {
  const start = Date.now();
  while (Date.now() - start < 10000) {
    stdin.send(key);
    try {
      return await waitFor(stdout, test, what, 1000);
    } catch {
      // not handled yet: send it again
    }
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
      await press(stdin, stdout, KEY.escape, (f) => !f.includes('type to search'), 'search box left');
      frame = await press(stdin, stdout, 'e', (f) => f.includes('package_main') && f.includes('src/server.js:1'), 'entry points');

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

  it('shows the code map, zooms in and out, and opens API endpoints with /show', async () => {
    const legacyData = tempDir('ui-legacy');
    const root = copyFixture('legacy-java', tempDir('fx-legacy'));
    const added = await runEngine('add_project', { name: 'legacy', path: root }, { enginePath: ENGINE, dataDir: legacyData });
    assert.equal(added.status, 'ok');
    const st = (await runEngine('project_status', { project: 'legacy' }, { enginePath: ENGINE, dataDir: legacyData })).result;
    const stdout = new FakeStdout();
    stdout.rows = 60;
    const stdin = new FakeStdin();
    const session = new EngineSession({ enginePath: ENGINE, dataDir: legacyData, timeoutMs: 15000 });
    const app = render(<App session={session} project="legacy" summary={st.index} rootPath={st.project.root_path} />, {
      stdout: stdout as any,
      stdin: stdin as any,
      debug: true,
      exitOnCtrlC: false,
      patchConsole: false,
    });
    const exitPromise = app.waitUntilExit();
    try {
      // The home screen draws the project's parts as boxes joined by arrows.
      let frame = await waitFor(stdout, (f) => f.includes('Code map · whole project') && f.includes('legacy/web'), 'code map');
      assert.match(frame, /legacy\/service/);
      assert.match(frame, /▼/);
      // The part where routes and startup come in is selected first.
      frame = await waitFor(stdout, (f) => f.includes('legacy/server  src/main/java/com/acme/legacy/server'), 'selected part details');
      assert.match(frame, /calls → /);
      stdin.send(KEY.enter); // a leaf directory: list its files
      frame = await waitFor(stdout, (f) => f.includes('Code map · src/main/java/com/acme/legacy/server') && f.includes('EmbeddedServer.java'), 'files of the part');
      stdin.send('\u007f'); // Backspace: zoom out
      await waitFor(stdout, (f) => f.includes('Code map · whole project'), 'back to the whole project');

      stdin.send('/');
      await waitFor(stdout, (f) => f.includes('type to search'), 'search focus');
      stdin.send('/show GET /api/users/42');
      frame = await waitFor(stdout, (f) => f.includes('API endpoints matching "GET /api/users/42"') && f.includes('UserServlet.doGet'), '/show results');
      stdin.send(KEY.enter);
      frame = await waitFor(stdout, (f) => f.includes('handler     com.acme.legacy.web.UserServlet.doGet'), 'route view');
      assert.match(frame, /AuthFilter/);

      stdin.send('h');
      await waitFor(stdout, (f) => f.includes('Search: /show GET /api/users/42') && !f.includes('handler     '), 'home with the query');
      await press(stdin, stdout, KEY.escape, (f) => !f.includes('type to search'), 'search box left');
      frame = await press(stdin, stdout, 'r', (f) => f.includes('API routes') && f.includes('→ OrderServlet.service'), 'routes list with handler methods');
      assert.match(frame, /GET {5}\/api\/users\/\*  → UserServlet\.doGet/);

      stdin.send('q');
      const exited = await Promise.race([exitPromise.then(() => true), new Promise<boolean>((r) => setTimeout(() => r(false), 5000))]);
      assert.equal(exited, true, 'app exits on q');
    } finally {
      app.unmount();
      await session.close();
    }
  });
});

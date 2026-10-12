import { Box, Text, useApp, useInput, useStdout } from 'ink';
import React, { useCallback, useEffect, useRef, useState } from 'react';

import { useLatest } from './hooks.js';

import type { EngineSession } from '../engine/client.js';
import { formatCount, truncate } from '../format.js';
import { INITIAL_MAP } from './codemap.js';
import { EntryPointsView, FileView, HELP_LINES, HomeView, type HomeState, RouteView, RoutesView, StatusView, SymbolView, type View } from './views.js';

export interface AppProps {
  session: EngineSession;
  project: string;
  /** `index` part of the engine's project_status result. */
  summary: any;
  rootPath: string;
  /** Open this route first (proj show <project> <endpoint>). */
  initialRoute?: number;
  /** Start with this text in the search box. */
  initialQuery?: string;
}

function crumb(v: View): string {
  switch (v.kind) {
    case 'home':
      return 'home';
    case 'symbol':
      return `symbol #${v.id}`;
    case 'file':
      return `file #${v.id}`;
    case 'route':
      return `route #${v.id}`;
    case 'entries':
      return 'entry points';
    case 'routes':
      return 'routes';
    case 'status':
      return 'status';
  }
}

export function App({ session, project, summary, rootPath, initialRoute, initialQuery }: AppProps): React.ReactElement {
  const { exit } = useApp();
  const { stdout } = useStdout();
  const [size, setSize] = useState({ columns: stdout?.columns || 100, rows: stdout?.rows || 30 });
  const [stack, setStack] = useState<View[]>(initialRoute !== undefined ? [{ kind: 'home' }, { kind: 'route', id: initialRoute }] : [{ kind: 'home' }]);
  const [help, setHelp] = useState(false);
  const [home, setHome] = useState<HomeState>({ query: initialQuery ?? '', typing: false, selected: 0, map: INITIAL_MAP });
  const typing = home.typing;
  const typingRef = useRef(false);
  const setTyping = useCallback((value: boolean) => {
    typingRef.current = value;
    setHome((s) => ({ ...s, typing: value }));
  }, []);

  useEffect(() => {
    if (!stdout) return;
    const onResize = () => setSize({ columns: stdout.columns || 100, rows: stdout.rows || 30 });
    stdout.on('resize', onResize);
    return () => {
      stdout.off('resize', onResize);
    };
  }, [stdout]);

  const view = stack[stack.length - 1]!;
  const open = useCallback((v: View) => setStack((s) => (s.length >= 100 ? [...s.slice(1), v] : [...s, v])), []);
  const back = () => setStack((s) => (s.length > 1 ? s.slice(0, -1) : s));

  const latest = useLatest({ help, view });
  useInput((input, key) => {
    const { help, view } = latest.current;
    const typing = typingRef.current;
    if (help) {
      if (input === '?' || key.escape || input === 'q') setHelp(false);
      return;
    }
    if (typing && view.kind === 'home') return; // the search box owns the keyboard
    if (input === 'q') return exit();
    if (input === '?') return setHelp(true);
    if (input === 'h') return setStack([{ kind: 'home' }]);
    if ((key.escape || key.backspace || key.delete || input === 'b') && view.kind !== 'home') back();
  });

  const width = Math.max(40, size.columns - 1);
  const bodyHeight = Math.max(6, size.rows - 5);
  const counts = summary?.counts ?? {};
  const status = summary?.analysis_status ?? 'unknown';
  const statusColor = status === 'success' ? 'green' : status === 'partial' || status === 'no_supported_files' ? 'yellow' : 'red';
  const props = { session, project, width, height: bodyHeight, active: !help, open };

  let body: React.ReactElement;
  if (help) {
    body = (
      <Box flexDirection="column">
        <Text bold>Keyboard shortcuts</Text>
        {HELP_LINES.slice(0, bodyHeight - 1).map((l, i) => (
          <Text key={i} wrap="truncate">
            {truncate(l, width)}
          </Text>
        ))}
      </Box>
    );
  } else {
    switch (view.kind) {
      case 'home':
        body = <HomeView {...props} state={home} setState={setHome} typingRef={typingRef} setTyping={setTyping} entryCount={counts.entry_points ?? 0} routeCount={counts.routes ?? 0} />;
        break;
      case 'symbol':
        body = <SymbolView key={`s${view.id}`} {...props} id={view.id} />;
        break;
      case 'file':
        body = <FileView key={`f${view.id}`} {...props} id={view.id} />;
        break;
      case 'route':
        body = <RouteView key={`r${view.id}`} {...props} id={view.id} />;
        break;
      case 'entries':
        body = <EntryPointsView {...props} />;
        break;
      case 'routes':
        body = <RoutesView {...props} />;
        break;
      case 'status':
        body = <StatusView {...props} summary={summary} />;
        break;
    }
  }

  const trail = stack.map(crumb).slice(-4).join(' > ');
  const hints = help
    ? '? or Esc close help'
    : typing && view.kind === 'home'
      ? 'type to search · Up/Down select · Enter open · Esc leave search'
      : view.kind === 'home'
        ? home.query.trim() === ''
          ? 'arrows select · Enter zoom/open · Backspace out · a links · / search · /show endpoint · e entries · r routes · ? help · q quit'
          : '/ search · Up/Down select · Enter open · Esc clear · e entries · r routes · s status · ? help · q quit'
        : 'Up/Down select · Enter open · Tab switch · Esc back · h home · ? help · q quit';
  return (
    <Box flexDirection="column" width={width}>
      <Box>
        <Text bold>{`proj · ${project} `}</Text>
        <Text color={statusColor}>{`[${status}] `}</Text>
        <Text dimColor wrap="truncate">{truncate(rootPath, Math.max(10, width - project.length - status.length - 12))}</Text>
      </Box>
      <Text dimColor wrap="truncate">
        {truncate(
          `files ${formatCount(counts.source_files)}/${formatCount(counts.files)} · symbols ${formatCount(counts.symbols)} · relationships ${formatCount(counts.relationships)} · routes ${formatCount(counts.routes)} · entry points ${formatCount(counts.entry_points)} · unresolved ${formatCount(counts.unresolved_edges)}`,
          width,
        )}
      </Text>
      <Text color="gray" wrap="truncate">{truncate(trail, width)}</Text>
      <Box flexDirection="column" height={bodyHeight}>
        {body}
      </Box>
      <Text dimColor wrap="truncate">{truncate(hints, width)}</Text>
    </Box>
  );
}

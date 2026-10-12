import { Box, Text, useInput } from 'ink';
import React, { useEffect, useMemo, useState } from 'react';

import type { EngineSession } from '../engine/client.js';
import { formatCount, glyph, truncate } from '../format.js';
import { clamp, ErrorLine, FlowTree, Loading, SelectList, Tabs } from './components.js';
import { useLatest, useQuery } from './hooks.js';
import { layoutTree, type SymbolBrief, type TraverseNode } from './tree.js';

export type View =
  | { kind: 'home' }
  | { kind: 'symbol'; id: number }
  | { kind: 'file'; id: number }
  | { kind: 'route'; id: number }
  | { kind: 'entries' }
  | { kind: 'routes' }
  | { kind: 'status' };

export interface ViewProps {
  session: EngineSession;
  project: string;
  width: number;
  height: number;
  active: boolean;
  open: (v: View) => void;
}

function loc(path: string, line?: number | null): string {
  return line ? `${path}:${line}` : path;
}

// ---------------------------------------------------------------------------
// Home: search
// ---------------------------------------------------------------------------

type ResultItem =
  | { type: 'route'; id: number; method: string; full_path: string; file: string; line: number; handler: string | null; status: string }
  | { type: 'symbol'; sym: SymbolBrief & { parent?: string | null; duplicate_name?: boolean; exported?: boolean } }
  | { type: 'file'; id: number; path: string; language: string | null; status: string };

function resultText(r: ResultItem, width: number): string {
  if (r.type === 'route') {
    const handler = r.handler ? `${glyph.arrow} ${r.handler}` : '';
    return `route   ${r.method} ${r.full_path}  ${handler}  ${loc(r.file, r.line)}${r.status !== 'resolved' ? ` [handler ${r.status}]` : ''}`;
  }
  if (r.type === 'file') return `file    ${r.path}  ${r.language ?? 'unsupported'}${r.status !== 'parsed' ? ` [${r.status}]` : ''}`;
  const s = r.sym;
  const kind = s.kind.padEnd(7).slice(0, 7);
  const ctx = s.qualified_name !== s.name ? `  (${s.qualified_name})` : '';
  return truncate(`${kind} ${s.name}  ${loc(s.path, s.line)}${ctx}`, width);
}

/** Search state lives in App so it survives navigating away and back. */
export interface HomeState {
  query: string;
  typing: boolean;
  selected: number;
}

export function HomeView(
  props: ViewProps & {
    state: HomeState;
    setState: (f: (s: HomeState) => HomeState) => void;
    /** Search focus, updated synchronously so back-to-back keys see the new mode. */
    typingRef: { current: boolean };
    setTyping: (typing: boolean) => void;
    entryCount: number;
    routeCount: number;
  },
) {
  const { query: queryText, typing, selected } = props.state;
  const setQueryText = (f: (q: string) => string) => props.setState((s) => ({ ...s, query: f(s.query), selected: 0 }));
  const setTyping = props.setTyping;
  const setSelected = (f: (n: number) => number) => props.setState((s) => ({ ...s, selected: f(s.selected) }));

  const trimmed = queryText.trim();
  const search = useQuery<any>(props.session, 'search', trimmed ? { project: props.project, query: trimmed, limit: 15 } : null, trimmed, 120);
  const items: ResultItem[] = useMemo(() => {
    const d = search.data;
    if (!d) return [];
    const out: ResultItem[] = [];
    for (const r of d.routes ?? []) {
      out.push({ type: 'route', id: r.id, method: r.method, full_path: r.full_path, file: r.file, line: r.line, handler: r.handler_name, status: r.handler_status });
    }
    for (const s of d.symbols ?? []) out.push({ type: 'symbol', sym: s });
    for (const f of d.files ?? []) out.push({ type: 'file', id: f.id, path: f.path, language: f.language, status: f.status });
    return out;
  }, [search.data]);
  const latest = useLatest({ items, state: props.state });
  const openSelected = () => {
    const { items, state } = latest.current;
    const item = items[state.selected];
    if (!item) return;
    if (item.type === 'route') props.open({ kind: 'route', id: item.id });
    else if (item.type === 'symbol') props.open({ kind: 'symbol', id: item.sym.id });
    else props.open({ kind: 'file', id: item.id });
  };

  useInput(
    (input, key) => {
      const { items, state } = latest.current;
      if (key.upArrow) return setSelected((s) => clamp(s - 1, 0, Math.max(0, items.length - 1)));
      if (key.downArrow) return setSelected((s) => clamp(s + 1, 0, Math.max(0, items.length - 1)));
      if (key.return) return openSelected();
      if (props.typingRef.current) {
        if (key.escape) return setTyping(false);
        if (key.backspace || key.delete) return setQueryText((q) => q.slice(0, -1));
        if (key.ctrl && input === 'u') return setQueryText(() => '');
        if (!key.ctrl && !key.meta && input && !key.tab) setQueryText((q) => (q + input).slice(0, 200));
        return;
      }
      if (input === '/') return setTyping(true);
      if (input === 'e') return props.open({ kind: 'entries' });
      if (input === 'r') return props.open({ kind: 'routes' });
      if (input === 's') return props.open({ kind: 'status' });
      if (key.escape && state.query) return setQueryText(() => '');
    },
    { isActive: props.active },
  );

  const counts = search.data
    ? `${(search.data.symbols ?? []).length} symbols, ${(search.data.files ?? []).length} files, ${(search.data.routes ?? []).length} routes`
    : '';
  return (
    <Box flexDirection="column">
      <Box>
        <Text bold color={typing ? 'cyan' : undefined}>{'Search: '}</Text>
        <Text>{queryText}</Text>
        {typing ? <Text inverse> </Text> : <Text dimColor>{queryText ? '' : ' press / to search symbols, files and routes'}</Text>}
      </Box>
      {trimmed === '' ? (
        <Box flexDirection="column" marginTop={1}>
          <Text>Examples: getUser · UserService.find · routes/users · GET /api/users/:id · /api/users/42</Text>
          <Text>{`  e  entry points (${formatCount(props.entryCount)})`}</Text>
          <Text>{`  r  API routes (${formatCount(props.routeCount)})`}</Text>
          <Text>{'  s  analysis status and known limitations'}</Text>
        </Box>
      ) : search.error ? (
        <ErrorLine message={search.error} />
      ) : (
        <Box flexDirection="column">
          <Text dimColor>{search.loading ? 'Searching...' : `${counts} (case-insensitive substring; exact names rank first)`}</Text>
          <SelectList items={items} selected={selected} height={props.height - 3} width={props.width} render={(r) => resultText(r, props.width)} empty="No matches." />
        </Box>
      )}
    </Box>
  );
}

// ---------------------------------------------------------------------------
// Flow panel shared by symbol and route views
// ---------------------------------------------------------------------------

function useFlow(props: ViewProps, symbolId: number | null, direction: 'forward' | 'backward', depth: number, unresolved: boolean) {
  return useQuery<any>(
    props.session,
    'traverse',
    symbolId === null
      ? null
      : { project: props.project, symbol_id: symbolId, direction, max_depth: depth, max_nodes: 300, include_unresolved: unresolved },
    `${symbolId}:${direction}:${depth}:${unresolved}`,
  );
}

function FlowPanel(props: ViewProps & { symbolId: number | null; direction: 'forward' | 'backward'; selected: number; setSelected: (n: number) => void; depth: number; unresolved: boolean; heightUsed: number }) {
  const flow = useFlow(props, props.symbolId, props.direction, props.depth, props.unresolved);
  const nodes: TraverseNode[] = flow.data?.nodes ?? [];
  const rows = useMemo(() => layoutTree(nodes), [flow.data]);
  const latest = useLatest({ rows, selected: props.selected, setSelected: props.setSelected });
  useInput(
    (_input, key) => {
      const { rows, selected, setSelected } = latest.current;
      if (key.upArrow) setSelected(clamp(selected - 1, 0, Math.max(0, rows.length - 1)));
      if (key.downArrow) setSelected(clamp(selected + 1, 0, Math.max(0, rows.length - 1)));
      if (key.return) {
        const row = rows[selected];
        const sym = row?.node.symbol;
        if (sym && row && row.node.depth > 0) props.open({ kind: 'symbol', id: sym.id });
      }
    },
    { isActive: props.active },
  );
  if (props.symbolId === null) return <Text dimColor>No symbol to trace.</Text>;
  if (flow.loading && !flow.data) return <Loading what="relationships" />;
  if (flow.error) return <ErrorLine message={flow.error} />;
  const notes: string[] = [];
  if (flow.data?.truncated) notes.push('result truncated by limits');
  if (flow.data?.hidden_unresolved) notes.push(`${flow.data.hidden_unresolved} unresolved/external hidden (u)`);
  return (
    <Box flexDirection="column">
      <FlowTree nodes={nodes} selected={props.selected} height={props.height - props.heightUsed} width={props.width} />
      {notes.length > 0 ? <Text dimColor>{notes.join(' · ')}</Text> : null}
    </Box>
  );
}

function ExceptionsPanel(props: ViewProps & { symbolId: number | null }) {
  const ex = useQuery<any>(props.session, 'exceptions', props.symbolId === null ? null : { project: props.project, symbol_id: props.symbolId, depth: 3 }, String(props.symbolId));
  if (props.symbolId === null) return <Text dimColor>No symbol.</Text>;
  if (ex.loading) return <Loading what="exception sites" />;
  if (ex.error) return <ErrorLine message={ex.error} />;
  const d = ex.data;
  const lines: { text: string; color?: string }[] = [];
  const site = (x: any) => `${x.kind} ${x.type ?? x.expression ?? ''}  ${loc(x.path, x.line)}`;
  lines.push({ text: `Explicit throw/raise sites in this symbol (${d.explicit.length}):` });
  for (const x of d.explicit) {
    const handled = x.handled_locally ? `  handled locally by catch at line ${x.handled_locally.catch_line} (${x.handled_locally.match})` : '';
    lines.push({ text: `  ${site(x)}${handled}` });
  }
  if (d.declared.length > 0) {
    lines.push({ text: `Declared (throws clause):` });
    for (const x of d.declared) lines.push({ text: `  ${x.type}  ${loc(x.path, x.line)}` });
  }
  lines.push({ text: `Recognized handlers in this symbol (${d.handlers.length}):` });
  for (const x of d.handlers) lines.push({ text: `  catch ${x.type ?? '*'}  ${loc(x.path, x.line)}` });
  lines.push({ text: `Possible: explicit sites in callees reachable through resolved calls (${d.in_callees.length}):`, color: 'yellow' });
  for (const x of d.in_callees) {
    lines.push({ text: `  ${x.type ?? x.expression ?? '?'} in ${x.in_symbol.qualified_name}  ${loc(x.path, x.line)}  depth ${x.call_depth}${x.handled_locally ? ' (caught there)' : ''}` });
  }
  lines.push({ text: 'Callee sites are statically reachable, not proof that the exception occurs at runtime.', color: 'gray' });
  return (
    <Box flexDirection="column">
      {lines.slice(0, Math.max(1, props.height - 4)).map((l, i) => (
        <Text key={i} color={l.color} wrap="truncate">
          {truncate(l.text, props.width)}
        </Text>
      ))}
    </Box>
  );
}

function SourcePanel(props: ViewProps & { fileId: number | null; line: number }) {
  const snip = useQuery<any>(
    props.session,
    'snippet',
    props.fileId === null ? null : { project: props.project, file_id: props.fileId, line: props.line, before: 2, after: Math.max(5, props.height - 8) },
    `${props.fileId}:${props.line}`,
  );
  if (snip.loading) return <Loading what="source" />;
  if (snip.error) return <ErrorLine message={snip.error} />;
  if (!snip.data) return <Text dimColor>No source.</Text>;
  if (snip.data.error) return <Text color="yellow">{snip.data.error}</Text>;
  return (
    <Box flexDirection="column">
      {snip.data.changed_since_index ? (
        <Text color="yellow">{`The file changed since it was indexed; line numbers may be off. Run: proj refresh ${props.project}`}</Text>
      ) : null}
      {snip.data.lines.map((l: any) => (
        <Text key={l.number} wrap="truncate" bold={l.number === props.line}>
          {truncate(`${String(l.number).padStart(5)} ${l.number === props.line ? glyph.sel : ' '} ${l.text}`, props.width)}
        </Text>
      ))}
    </Box>
  );
}

// ---------------------------------------------------------------------------
// Symbol view
// ---------------------------------------------------------------------------

const SYMBOL_TABS = ['Callees', 'Callers', 'Exceptions', 'Source'];

export function SymbolView(props: ViewProps & { id: number }) {
  const sym = useQuery<any>(props.session, 'symbol', { project: props.project, symbol_id: props.id }, String(props.id));
  const [tab, setTab] = useState(0);
  const [depth, setDepth] = useState(3);
  const [unresolved, setUnresolved] = useState(true);
  const [selected, setSelected] = useState(0);
  useEffect(() => setSelected(0), [tab, depth, unresolved]);
  const latestSym = useLatest(sym.data);
  useInput(
    (input, key) => {
      const data = latestSym.current;
      if (key.tab || key.rightArrow) setTab((t) => (t + 1) % SYMBOL_TABS.length);
      if (key.leftArrow) setTab((t) => (t + SYMBOL_TABS.length - 1) % SYMBOL_TABS.length);
      if (input === '+' || input === '=') setDepth((d) => Math.min(d + 1, 12));
      if (input === '-') setDepth((d) => Math.max(d - 1, 1));
      if (input === 'u') setUnresolved((u) => !u);
      if (input === 'f' && data) props.open({ kind: 'file', id: data.file_id });
    },
    { isActive: props.active },
  );
  if (sym.loading && !sym.data) return <Loading what="symbol" />;
  if (sym.error) return <ErrorLine message={sym.error} />;
  const s = sym.data;
  const outgoing = Object.entries(s.outgoing ?? {}).map(([k, v]) => `${v} ${k}`).join(', ') || 'none';
  const incoming = Object.entries(s.incoming ?? {}).map(([k, v]) => `${v} ${k}`).join(', ') || 'none';
  const details = [
    s.parent ? `in ${s.parent.kind} ${s.parent.qualified_name}` : null,
    s.exported ? 'exported' : null,
    s.visibility,
    s.is_static ? 'static' : null,
    s.is_definition === false ? 'declaration' : null,
    s.param_count !== null && s.param_count !== undefined ? `${s.param_count} params` : null,
  ].filter(Boolean);
  const routes = (s.routes ?? []).map((r: any) => `${r.method} ${r.full_path}`).join(', ');
  const entries = (s.entry_points ?? []).map((e: any) => e.kind).join(', ');
  const header = 5 + (routes ? 1 : 0) + (entries ? 1 : 0);
  return (
    <Box flexDirection="column">
      <Text bold wrap="truncate">{truncate(`${s.kind} ${s.qualified_name}`, props.width)}</Text>
      <Text wrap="truncate">{truncate(`${loc(s.path, s.line)}-${s.end_line}  ${s.language}  ${details.join(' · ')}`, props.width)}</Text>
      <Text dimColor wrap="truncate">{truncate(`outgoing: ${outgoing}   incoming: ${incoming}`, props.width)}</Text>
      {routes ? <Text color="cyan" wrap="truncate">{truncate(`handles: ${routes}`, props.width)}</Text> : null}
      {entries ? <Text color="green">{`entry point: ${entries}`}</Text> : null}
      <Tabs tabs={SYMBOL_TABS} active={tab} right={tab < 2 ? `depth ${depth} (+/-) · unresolved ${unresolved ? 'shown' : 'hidden'} (u) · f file` : 'f file'} />
      {tab === 0 || tab === 1 ? (
        <FlowPanel {...props} symbolId={props.id} direction={tab === 0 ? 'forward' : 'backward'} selected={selected} setSelected={setSelected} depth={depth} unresolved={unresolved} heightUsed={header + 1} />
      ) : tab === 2 ? (
        <ExceptionsPanel {...props} symbolId={props.id} height={props.height - header} />
      ) : (
        <SourcePanel {...props} fileId={s.file_id} line={s.line} height={props.height - header} />
      )}
    </Box>
  );
}

// ---------------------------------------------------------------------------
// File view
// ---------------------------------------------------------------------------

const FILE_TABS = ['Symbols', 'Dependencies', 'Dependents'];

export function FileView(props: ViewProps & { id: number }) {
  const file = useQuery<any>(props.session, 'file', { project: props.project, file_id: props.id, symbol_limit: 300 }, String(props.id));
  const [tab, setTab] = useState(0);
  const [selected, setSelected] = useState(0);
  const deps = useQuery<any>(
    props.session,
    'file_deps',
    tab === 0 ? null : { project: props.project, file_id: props.id, direction: tab === 1 ? 'out' : 'in', limit: 300 },
    `${props.id}:${tab}`,
  );
  useEffect(() => setSelected(0), [tab]);
  const items: any[] = tab === 0 ? (file.data?.symbols ?? []) : (deps.data?.dependencies ?? []);
  const latest = useLatest({ items, selected, tab });
  useInput(
    (_input, key) => {
      const { items, selected, tab } = latest.current;
      if (key.tab || key.rightArrow) setTab((t) => (t + 1) % FILE_TABS.length);
      if (key.leftArrow) setTab((t) => (t + FILE_TABS.length - 1) % FILE_TABS.length);
      if (key.upArrow) setSelected((s) => clamp(s - 1, 0, Math.max(0, items.length - 1)));
      if (key.downArrow) setSelected((s) => clamp(s + 1, 0, Math.max(0, items.length - 1)));
      if (key.return) {
        const it = items[selected];
        if (!it) return;
        if (tab === 0) props.open({ kind: 'symbol', id: it.id });
        else {
          const other = tab === 1 ? it.target : it.source;
          if (other) props.open({ kind: 'file', id: other.id });
        }
      }
    },
    { isActive: props.active },
  );
  if (file.loading && !file.data) return <Loading what="file" />;
  if (file.error) return <ErrorLine message={file.error} />;
  const f = file.data;
  const render = (it: any) => {
    if (tab === 0) return `${'  '.repeat(it.parent_id && it.parent_id !== f.module_symbol_id ? 1 : 0)}${it.kind.padEnd(8)} ${it.name}  :${it.line}`;
    const other = tab === 1 ? it.target : it.source;
    const where = other ? other.path : it.specifier;
    const status = it.status === 'resolved' ? '' : ` [${it.status}${it.note ? ': ' + it.note : ''}]`;
    return `${it.kind.padEnd(14)} ${where}  :${it.line}${tab === 1 && other ? `  (${it.specifier})` : ''}${status}`;
  };
  const errors = f.error_count > 0 ? ` · ${f.error_count} syntax errors (first at line ${f.first_error_line})` : '';
  return (
    <Box flexDirection="column">
      <Text bold wrap="truncate">{truncate(`file ${f.path}`, props.width)}</Text>
      <Text wrap="truncate">{`${f.language ?? 'unsupported'} · ${f.status} · ${formatCount(f.line_count)} lines${errors}${f.module_name ? ` · module ${f.module_name}` : ''}`}</Text>
      <Tabs tabs={[`Symbols (${f.symbols.length}${f.symbols_truncated ? '+' : ''})`, `Dependencies (${f.dependency_count})`, `Dependents (${f.dependent_count})`]} active={tab} />
      {tab !== 0 && deps.loading ? (
        <Loading what="dependencies" />
      ) : tab !== 0 && deps.error ? (
        <ErrorLine message={deps.error} />
      ) : (
        <SelectList
          items={items}
          selected={selected}
          height={props.height - 5}
          width={props.width}
          render={render}
          color={(it) => (tab !== 0 && it.status !== 'resolved' ? (it.status === 'external' ? 'cyan' : 'yellow') : undefined)}
          empty={tab === 0 ? 'No symbols in this file.' : 'None.'}
        />
      )}
    </Box>
  );
}

// ---------------------------------------------------------------------------
// Route view
// ---------------------------------------------------------------------------

const ROUTE_TABS = ['Flow', 'Callers', 'Exceptions'];

export function RouteView(props: ViewProps & { id: number }) {
  const [depth, setDepth] = useState(4);
  const [tab, setTab] = useState(0);
  const [unresolved, setUnresolved] = useState(true);
  const [selected, setSelected] = useState(0);
  const flow = useQuery<any>(props.session, 'route_flow', { project: props.project, route_id: props.id, max_depth: 1, max_nodes: 1 }, String(props.id));
  useEffect(() => setSelected(0), [tab, depth, unresolved]);
  const latestFlow = useLatest(flow.data);
  useInput(
    (input, key) => {
      if (key.tab || key.rightArrow) setTab((t) => (t + 1) % ROUTE_TABS.length);
      if (key.leftArrow) setTab((t) => (t + ROUTE_TABS.length - 1) % ROUTE_TABS.length);
      if (input === '+' || input === '=') setDepth((d) => Math.min(d + 1, 12));
      if (input === '-') setDepth((d) => Math.max(d - 1, 1));
      if (input === 'u') setUnresolved((u) => !u);
      const h = latestFlow.current?.route?.handler?.symbol;
      if (input === 'o' && h) props.open({ kind: 'symbol', id: h.id });
    },
    { isActive: props.active },
  );
  if (flow.loading && !flow.data) return <Loading what="route" />;
  if (flow.error) return <ErrorLine message={flow.error} />;
  const r = flow.data.route;
  const handler = r.handler.symbol;
  const lines: { text: string; color?: string }[] = [];
  lines.push({ text: `registered  ${loc(r.registration.path, r.registration.line)} on ${r.registration.registrar}  (${r.framework}, ${r.confidence})` });
  if (r.mount) lines.push({ text: `mounted     ${r.mount.prefix ?? ''} via ${r.mount.registrar}.use(...) at ${loc(r.mount.path, r.mount.line)}` });
  else if (r.mount_status !== 'none') lines.push({ text: `prefix      ${r.mount_status}${r.mount_prefix ? ` (${r.mount_prefix})` : ''}`, color: r.mount_status === 'resolved' || r.mount_status === 'class_mapping' ? undefined : 'yellow' });
  lines.push(
    handler
      ? { text: `handler     ${handler.qualified_name}  ${loc(handler.path, handler.line)}  [${r.handler.status}]${r.handler.note ? ` ${r.handler.note}` : ''}` }
      : { text: `handler     not resolved: ${r.handler.text ?? '?'} [${r.handler.status}]${r.handler.note ? ` — ${r.handler.note}` : ''}`, color: 'red' },
  );
  if (r.middleware.length > 0) {
    lines.push({ text: `middleware  ${r.middleware.map((m: any) => `${m.scope}:${m.text}${m.status === 'resolved' ? '' : ` [${m.status}]`}`).join(' → ')}` });
  } else {
    lines.push({ text: 'middleware  none recognized', color: 'gray' });
  }
  const st = flow.data.startup;
  if (st?.found) {
    const chain = st.path.map((p: any) => p.symbol?.qualified_name ?? '?').join(` ${glyph.arrow} `);
    lines.push({ text: `startup     ${chain}  (entry: ${st.entry_kind})` });
  } else if (st) {
    lines.push({ text: `startup     ${st.message}`, color: 'yellow' });
  }
  const used = 2 + lines.length + 1;
  return (
    <Box flexDirection="column">
      <Text bold wrap="truncate">{truncate(`${r.method} ${r.full_path}`, props.width)}</Text>
      {r.path !== r.full_path ? <Text dimColor>{`declared as ${r.path}`}</Text> : null}
      {lines.map((l, i) => (
        <Text key={i} color={l.color} wrap="truncate">
          {truncate(l.text, props.width)}
        </Text>
      ))}
      <Tabs tabs={ROUTE_TABS} active={tab} right={`depth ${depth} (+/-) · unresolved ${unresolved ? 'shown' : 'hidden'} (u) · o open handler`} />
      {!handler ? (
        <Text color="yellow">{flow.data.message ?? 'No call flow: the handler is not resolved.'}</Text>
      ) : tab === 2 ? (
        <ExceptionsPanel {...props} symbolId={handler.id} height={props.height - used} />
      ) : (
        <FlowPanel {...props} symbolId={handler.id} direction={tab === 0 ? 'forward' : 'backward'} selected={selected} setSelected={setSelected} depth={depth} unresolved={unresolved} heightUsed={used + 1} />
      )}
    </Box>
  );
}

// ---------------------------------------------------------------------------
// Lists: entry points and routes
// ---------------------------------------------------------------------------

export function EntryPointsView(props: ViewProps) {
  const res = useQuery<any>(props.session, 'entry_points', { project: props.project, limit: 200 }, 'entries');
  const [selected, setSelected] = useState(0);
  const items: any[] = res.data?.entry_points ?? [];
  const latest = useLatest({ items, selected });
  useInput(
    (_i, key) => {
      const { items, selected } = latest.current;
      if (key.upArrow) setSelected((s) => clamp(s - 1, 0, Math.max(0, items.length - 1)));
      if (key.downArrow) setSelected((s) => clamp(s + 1, 0, Math.max(0, items.length - 1)));
      if (key.return && items[selected]?.symbol) props.open({ kind: 'symbol', id: items[selected].symbol.id });
    },
    { isActive: props.active },
  );
  if (res.loading) return <Loading what="entry points" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <Text bold>Entry points</Text>
      <Text dimColor>Enter traces forward from the entry (startup, module loads, calls, mounts, route registration).</Text>
      {items.length === 0 ? <Text color="yellow">{res.data?.message ?? 'No entry points recognized.'}</Text> : null}
      <SelectList
        items={items}
        selected={selected}
        height={props.height - 3}
        width={props.width}
        render={(e) => `${e.kind.padEnd(15)} ${loc(e.path, e.line)}  ${e.reason}${e.symbol && e.symbol.kind !== 'module' ? `  (${e.symbol.qualified_name})` : ''}`}
      />
    </Box>
  );
}

export function RoutesView(props: ViewProps) {
  const res = useQuery<any>(props.session, 'routes', { project: props.project, limit: 500 }, 'routes');
  const [selected, setSelected] = useState(0);
  const items: any[] = res.data?.routes ?? [];
  const latest = useLatest({ items, selected });
  useInput(
    (_i, key) => {
      const { items, selected } = latest.current;
      if (key.upArrow) setSelected((s) => clamp(s - 1, 0, Math.max(0, items.length - 1)));
      if (key.downArrow) setSelected((s) => clamp(s + 1, 0, Math.max(0, items.length - 1)));
      if (key.return && items[selected]) props.open({ kind: 'route', id: items[selected].id });
    },
    { isActive: props.active },
  );
  if (res.loading) return <Loading what="routes" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <Text bold>{`API routes (${items.length}${res.data?.has_more ? '+' : ''})`}</Text>
      <SelectList
        items={items}
        selected={selected}
        height={props.height - 2}
        width={props.width}
        render={(r) =>
          `${r.method.padEnd(7)} ${r.full_path}  ${glyph.arrow} ${r.handler_name ?? '?'}${r.handler_status !== 'resolved' ? ` [${r.handler_status}]` : ''}  ${loc(r.file, r.line)}  ${r.framework}${r.mount_status === 'unmounted' || r.mount_status === 'unknown_registrar' ? ` [prefix ${r.mount_status}]` : ''}`
        }
        color={(r) => (r.handler_status !== 'resolved' ? 'yellow' : undefined)}
        empty="No routes were recognized for the supported frameworks."
      />
    </Box>
  );
}

// ---------------------------------------------------------------------------
// Status and limitations
// ---------------------------------------------------------------------------

export function StatusView(props: ViewProps & { summary: any }) {
  const s = props.summary;
  const st = s?.stats ?? {};
  const files = st.files ?? {};
  const calls = st.resolution?.calls ?? {};
  const deps = st.resolution?.dependencies ?? {};
  const lines = [
    `Analysis status: ${s?.analysis_status ?? 'unknown'}   indexed ${s?.indexed_at ?? '-'}   run ${s?.run_id ?? '-'}`,
    `Files: ${formatCount(files.discovered)} discovered · ${formatCount(files.parsed)} parsed · ${formatCount(files.with_syntax_errors)} with syntax errors · ${formatCount(files.parse_failed)} failed · ${formatCount(files.skipped)} skipped · ${formatCount(files.unsupported)} unsupported · ${formatCount(files.excluded)} excluded`,
    `Languages: ${Object.entries(s?.languages ?? {}).map(([k, v]) => `${k} ${v}`).join(', ') || 'none'}`,
    `Calls: ${formatCount(calls.resolved)} resolved · ${formatCount(calls.external)} external · ${formatCount(calls.ambiguous)} ambiguous · ${formatCount(calls.heuristic)} heuristic · ${formatCount(calls.unresolved)} unresolved`,
    `File dependencies: ${formatCount(deps.resolved)} local · ${formatCount(deps.external)} external · ${formatCount(deps.unresolved)} unresolved`,
    '',
    'Known limitations (see docs/LANGUAGE_SUPPORT.md):',
    ' - Relationships are statically discovered possibilities, not traced runtime behavior.',
    ' - Dynamic dispatch, callbacks, reflection, dependency injection and computed names are often unresolved.',
    ' - "heuristic" edges were matched by name only; "ambiguous" edges list every candidate.',
    ' - Exceptions: only explicit throw/raise sites; callee sites are possible, not guaranteed.',
    ' - Routes: Express-style, NestJS, Flask, FastAPI, Django path(), Spring MVC and JAX-RS patterns only.',
    ' - tsconfig path aliases, Python sys.path changes and C/C++ build include paths are not read.',
  ];
  useInput(() => undefined, { isActive: props.active });
  return (
    <Box flexDirection="column">
      <Text bold>Analysis status and limitations</Text>
      {lines.map((l, i) => (
        <Text key={i} wrap="truncate">
          {truncate(l, props.width)}
        </Text>
      ))}
    </Box>
  );
}

export const HELP_LINES = [
  'Global',
  '  /            focus search (on the home screen)',
  '  Esc          leave the search box / go back',
  '  Backspace, b go back (when not typing)',
  '  h            home',
  '  ?            toggle this help',
  '  q, Ctrl+C    quit (q types normally inside the search box)',
  'Lists and trees',
  '  Up/Down      move selection',
  '  Enter        open the selected symbol, file or route',
  'Symbol and route views',
  '  Tab, Left/Right  switch tab (callees/callers/exceptions/source)',
  '  + / -        increase / decrease traversal depth',
  '  u            show or hide unresolved and external calls',
  '  f            open the containing file (symbol view)',
  '  o            open the handler symbol (route view)',
  'Home',
  '  e            entry points     r  API routes     s  status and limitations',
  'Markers: [unresolved] [ambiguous] [heuristic] [external]; "shown above" marks an already expanded node (cycle).',
];

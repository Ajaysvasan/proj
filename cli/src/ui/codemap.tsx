// Code map on the home screen: the project's main parts (directory groups
// computed by the engine's `overview` query) drawn as a graph of boxes and
// arrows, with zoom into a part and a details panel for the selected one.
import { Box, Text, useInput } from 'ink';
import React, { useMemo } from 'react';

import { formatCount, glyph, truncate, UNICODE } from '../format.js';
import { clamp, ErrorLine, Loading, SelectList } from './components.js';
import { type CellStyle, type GraphNode, neighborBox, renderGraph } from './graph.js';
import { useLatest, useQuery } from './hooks.js';
import type { ViewProps } from './views.js';

export interface MapFrame {
  focus: string;
  /** List only the files directly in `focus` (a "files" group). */
  direct: boolean;
}

export interface MapState {
  stack: MapFrame[];
  node: number | null;
  file: number;
  /** Links drawn per part: 1 (backbone), 3, or all. */
  links: number;
}

export const INITIAL_MAP: MapState = { stack: [{ focus: '', direct: false }], node: null, file: 0, links: 1 };

const STYLE: Record<CellStyle, { color?: string; bold?: boolean; dim?: boolean }> = {
  none: {},
  edge: { color: 'gray' },
  edgeHot: { color: 'cyan', bold: true },
  arrow: { color: 'gray' },
  arrowHot: { color: 'cyan', bold: true },
  box: {},
  boxSel: { color: 'cyan', bold: true },
  label: { bold: true },
  labelSel: { color: 'cyan', bold: true },
  detail: { dim: true },
  entry: { color: 'green', bold: true },
  dim: { color: 'gray' },
};

function plural(n: number, one: string, many = `${one}s`): string {
  return `${formatCount(n)} ${n === 1 ? one : many}`;
}

function boxDetail(n: any): string {
  const parts = [plural(n.files, 'file')];
  if (n.routes_declared > 0) parts.push(plural(n.routes_declared, 'route'));
  else if (n.entry_points > 0) parts.push(plural(n.entry_points, 'entry', 'entries'));
  else if (n.route_handlers > 0) parts.push(plural(n.route_handlers, 'handler'));
  return parts.join(' · ');
}

function maxNodesFor(width: number): number {
  if (width >= 170) return 16;
  if (width >= 130) return 14;
  if (width >= 100) return 11;
  return 8;
}

export function CodeMap(props: ViewProps & { state: MapState; setState: (f: (s: MapState) => MapState) => void }) {
  const { state, setState } = props;
  const frame = state.stack[state.stack.length - 1]!;
  const maxNodes = maxNodesFor(props.width);
  const res = useQuery<any>(
    props.session,
    'overview',
    { project: props.project, focus: frame.focus, max_nodes: maxNodes, direct_files: frame.direct },
    `${frame.focus}|${frame.direct}|${maxNodes}`,
  );
  const data = res.data;
  const nodes: any[] = data?.nodes ?? [];
  const files: any[] | null = data?.files && nodes.length <= 2 ? data.files : null;
  // Start on the part with the most routes and entry points: where requests and startup come in.
  const start = nodes.reduce<any>((best, n) => {
    const weight = (x: any) => (x ? x.routes_declared + x.entry_points : -1);
    return weight(n) > weight(best) ? n : best;
  }, null);
  const selected = state.node !== null && nodes.some((n) => n.id === state.node) ? state.node : (start?.id ?? null);

  const graph = useMemo(() => {
    if (!data || files) return null;
    const gnodes: GraphNode[] = nodes.map((n) => ({
      id: n.id,
      label: n.label,
      detail: n.kind === 'outside' ? 'calls in/out of here' : boxDetail(n),
      tone: n.kind === 'outside' ? 'outside' : n.kind === 'other' ? 'other' : n.entry_points > 0 || n.routes_declared > 0 ? 'entry' : 'plain',
    }));
    const gedges = (data.edges ?? []).map((e: any) => ({ from: e.from, to: e.to, weight: e.count }));
    return renderGraph(gnodes, gedges, { width: props.width, selected, maxEdgesPerNode: state.links, unicode: UNICODE });
  }, [data, files, selected, state.links, props.width]);

  const latest = useLatest({ graph, nodes, files, selected, state });
  useInput(
    (input, key) => {
      const { graph, nodes, files, selected, state } = latest.current;
      const zoomOut = () => {
        if (state.stack.length > 1) setState((s) => ({ ...s, stack: s.stack.slice(0, -1), node: null, file: 0 }));
      };
      if (key.escape || key.backspace || key.delete || input === 'b') return zoomOut();
      if (files) {
        if (key.upArrow) setState((s) => ({ ...s, file: clamp(s.file - 1, 0, Math.max(0, files.length - 1)) }));
        if (key.downArrow) setState((s) => ({ ...s, file: clamp(s.file + 1, 0, Math.max(0, files.length - 1)) }));
        if (key.return && files[state.file]) props.open({ kind: 'file', id: files[state.file].id });
        return;
      }
      if (!graph) return;
      const move = (dir: 'left' | 'right' | 'up' | 'down') => setState((s) => ({ ...s, node: neighborBox(graph.boxes, selected, dir) }));
      if (key.leftArrow) return move('left');
      if (key.rightArrow) return move('right');
      if (key.upArrow) return move('up');
      if (key.downArrow) return move('down');
      if (input === 'a') return setState((s) => ({ ...s, links: s.links === 1 ? 3 : s.links === 3 ? 99 : 1 }));
      if (key.return) {
        const n = nodes.find((x) => x.id === selected);
        if (!n) return;
        if (n.kind === 'outside') return setState((s) => ({ ...s, stack: s.stack.slice(0, 1), node: null, file: 0 }));
        if (n.kind === 'other') return;
        setState((s) => ({ ...s, stack: [...s.stack, { focus: n.path, direct: n.kind === 'files' }], node: null, file: 0 }));
      }
    },
    { isActive: props.active },
  );

  const where = frame.focus === '' ? 'whole project' : frame.focus + (frame.direct ? ' (files directly here)' : '');
  const title = `Code map · ${where}`;
  if (res.loading && !data) return <Loading what="code map" />;
  if (res.error) return <ErrorLine message={res.error} />;
  if (!data) return <Text dimColor>No data.</Text>;

  if (files) {
    return (
      <Box flexDirection="column">
        <Text bold wrap="truncate">{truncate(`${title} · ${plural(files.length, 'file')}`, props.width)}</Text>
        <SelectList
          items={files}
          selected={state.file}
          height={props.height - 2}
          width={props.width}
          render={(f) => `${f.path}  ${f.language || 'unsupported'}  ${plural(f.symbols, 'symbol')}`}
          empty="No analyzed files here."
        />
        <Text dimColor>Enter open file · Backspace back to the map</Text>
      </Box>
    );
  }
  if (!graph || nodes.length === 0) return <Text dimColor>{data.message ?? 'Nothing to draw.'}</Text>;

  // Details of the selected part, including links that are not drawn.
  const node = nodes.find((n) => n.id === selected);
  const label = (id: number) => nodes.find((n) => n.id === id)?.label ?? '?';
  const outLinks = (data.edges as any[]).filter((e) => e.from === selected).sort((a, b) => b.count - a.count);
  const inLinks = (data.edges as any[]).filter((e) => e.to === selected).sort((a, b) => b.count - a.count);
  const list = (links: any[], end: 'to' | 'from') =>
    links.length === 0 ? 'none' : links.slice(0, 5).map((e) => `${label(e[end])} (${formatCount(e.count)})`).join(', ') + (links.length > 5 ? ', ...' : '');
  const details: { text: string; bold?: boolean; color?: string }[] = [];
  if (node) {
    details.push({ text: `${node.label}  ${node.path || (node.kind === 'outside' ? 'code outside this part' : 'project root')}`, bold: true });
    if (node.kind !== 'outside') {
      const langs = Object.entries(node.languages ?? {}).map(([k, v]) => `${k} ${v}`).join(', ');
      details.push({
        text: `${plural(node.files, 'file')} · ${plural(node.symbols, 'symbol')}${langs ? ` (${langs})` : ''} · ${plural(node.entry_points, 'entry point')} · ${plural(node.routes_declared, 'route')} declared · ${plural(node.route_handlers, 'route handler')}`,
      });
    }
    details.push({ text: `calls ${glyph.arrow} ${list(outLinks, 'to')}` });
    details.push({ text: `called by ${glyph.back} ${list(inLinks, 'from')}` });
    const action =
      node.kind === 'outside' ? 'Enter: back to the whole project' : node.kind === 'other' ? 'grouped directories (a wider terminal shows more parts)' : node.kind === 'files' ? 'Enter: list these files' : node.drill ? `Enter: zoom into ${node.label}` : 'Enter: list its files';
    details.push({ text: action, color: 'gray' });
  }
  const linkMode = state.links === 1 ? 'strongest link per part' : state.links === 3 ? 'top 3 links per part' : 'all links';
  const graphHeight = Math.max(4, props.height - 2 - details.length);
  const box = graph.boxes.find((b) => b.id === selected);
  const top = box ? clamp(box.y - Math.floor((graphHeight - box.h) / 2), 0, Math.max(0, graph.height - graphHeight)) : 0;
  const window = graph.lines.slice(top, top + graphHeight);
  return (
    <Box flexDirection="column">
      <Text wrap="truncate">
        <Text bold>{truncate(title, Math.max(10, props.width - 60))}</Text>
        <Text dimColor>{`  ${linkMode} (a)${graph.hiddenEdges > 0 ? ` · ${graph.hiddenEdges} more links in the details` : ''}${graph.height > graphHeight ? ` · rows ${top + 1}-${Math.min(graph.height, top + graphHeight)} of ${graph.height}` : ''}`}</Text>
      </Text>
      {window.map((segs, i) => (
        <Text key={top + i} wrap="truncate">
          {segs.length === 0
            ? ' '
            : segs.map((s, j) => (
                <Text key={j} color={STYLE[s.style].color} bold={STYLE[s.style].bold} dimColor={STYLE[s.style].dim}>
                  {s.text}
                </Text>
              ))}
        </Text>
      ))}
      {details.map((d, i) => (
        <Text key={`d${i}`} bold={d.bold} color={d.color} wrap="truncate">
          {truncate(d.text, props.width)}
        </Text>
      ))}
    </Box>
  );
}

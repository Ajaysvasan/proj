// Pure helpers that turn engine traversal results into printable tree rows.
import { glyph } from '../format.js';

export interface SymbolBrief {
  id: number;
  name: string;
  qualified_name: string;
  kind: string;
  path: string;
  line: number;
}

export interface TraverseNode {
  index: number;
  parent: number | null;
  depth: number;
  symbol: SymbolBrief | null;
  edge: {
    kind: string;
    status: string;
    line: number;
    path: string;
    target_text?: string | null;
    note?: string | null;
    sites?: number;
  } | null;
  seen?: boolean;
  truncated_children?: number;
  candidates?: SymbolBrief[];
}

export interface TreeRow {
  node: TraverseNode;
  prefix: string;
}

/** Depth-first rows (in engine order) with box-drawing prefixes. Bounded by the input size. */
export function layoutTree(nodes: TraverseNode[]): TreeRow[] {
  const children = new Map<number, number[]>();
  for (const n of nodes) {
    if (n.parent === null) continue;
    const list = children.get(n.parent) ?? [];
    list.push(n.index);
    children.set(n.parent, list);
  }
  const rows: TreeRow[] = [];
  const byIndex = new Map(nodes.map((n) => [n.index, n]));
  // Explicit stack instead of recursion: depth is bounded by the engine, but stay safe.
  const stack: { index: number; prefix: string; last: boolean; root: boolean }[] = [];
  const root = nodes.find((n) => n.parent === null);
  if (!root) return rows;
  stack.push({ index: root.index, prefix: '', last: true, root: true });
  while (stack.length > 0) {
    const item = stack.pop()!;
    const node = byIndex.get(item.index);
    if (!node) continue;
    rows.push({ node, prefix: item.root ? '' : item.prefix + (item.last ? glyph.last : glyph.branch) });
    const kids = children.get(node.index) ?? [];
    const childPrefix = item.root ? '' : item.prefix + (item.last ? glyph.space : glyph.pipe);
    for (let i = kids.length - 1; i >= 0; i--) {
      stack.push({ index: kids[i]!, prefix: childPrefix, last: i === kids.length - 1, root: false });
    }
  }
  return rows;
}

const KIND_LABEL: Record<string, string> = {
  calls: '',
  instantiates: 'new ',
  registers_route: 'route ',
  uses_middleware: 'middleware ',
  mounts: 'mounts ',
  loads_module: 'loads ',
  overridden_by: 'override ',
};

export function edgeLabel(kind: string | undefined): string {
  if (!kind) return '';
  return KIND_LABEL[kind] ?? `${kind} `;
}

/** Plain-text description of a tree row (without colors), used by the UI and tests. */
export function describeRow(row: TreeRow): string {
  const n = row.node;
  const e = n.edge;
  const label = e ? edgeLabel(e.kind) : '';
  if (n.symbol) {
    const sites = e && e.sites && e.sites > 1 ? ` x${e.sites}` : '';
    const seen = n.seen ? ` ${glyph.seen} shown above` : '';
    const status = e && e.status !== 'resolved' ? ` [${e.status}]` : '';
    return `${label}${n.symbol.qualified_name}  ${n.symbol.path}:${n.symbol.line}${status}${sites}${seen}`;
  }
  const target = e?.target_text ?? '?';
  if (e?.status === 'ambiguous') {
    const names = (n.candidates ?? []).map((c) => `${c.qualified_name} (${c.path}:${c.line})`).join(', ');
    return `${label}${target} [ambiguous: ${names || 'several candidates'}]`;
  }
  return `${label}${target} [${e?.status ?? 'unknown'}${e?.note ? ': ' + e.note : ''}]`;
}

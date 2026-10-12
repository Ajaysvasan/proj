// Terminal rendering of a small directed graph (the code map): a layered
// layout with sources at the top, boxes for nodes, and box-drawing lines with
// arrowheads for edges. Pure functions, so layout and drawing are unit-tested
// without a terminal.
//
// Layout: cycles are broken by a depth-first search from the most "upstream"
// nodes (entry points first), layers come from the longest path, rows wider
// than the terminal wrap into extra layers, long edges get one-column dummy
// nodes in the layers they cross, and each layer is ordered by the barycenter
// of its neighbors to reduce crossings. Edges leave a box through one stem per
// direction and run along a horizontal "bus" in the gap below it; buses that
// would overlap get separate lanes.

export interface GraphNode {
  id: number;
  label: string;
  detail: string;
  tone?: 'entry' | 'outside' | 'other' | 'plain';
}

export interface GraphEdge {
  from: number;
  to: number;
  weight: number;
}

export type CellStyle = 'none' | 'edge' | 'edgeHot' | 'arrow' | 'arrowHot' | 'box' | 'boxSel' | 'label' | 'labelSel' | 'detail' | 'entry' | 'dim';

export interface Segment {
  text: string;
  style: CellStyle;
}

export interface BoxPos {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  layer: number;
  order: number;
}

export interface GraphRender {
  width: number;
  height: number;
  lines: Segment[][];
  boxes: BoxPos[];
  /** Edges left out to keep the drawing readable (see maxEdgesPerNode). */
  hiddenEdges: number;
}

export interface GraphOptions {
  width: number;
  selected?: number | null;
  /** Strongest outgoing edges drawn per node; all edges of the selected node are always drawn. */
  maxEdgesPerNode?: number;
  unicode?: boolean;
}

const BOX_H = 4;
const GAP_X = 3;
const MIN_W = 12;
const MAX_W = 28;

// Line connections per cell.
const UP = 1;
const DOWN = 2;
const LEFT = 4;
const RIGHT = 8;

const LINE_UNICODE: Record<number, string> = {
  [UP]: '│', [DOWN]: '│', [UP | DOWN]: '│', [LEFT]: '─', [RIGHT]: '─', [LEFT | RIGHT]: '─',
  [DOWN | RIGHT]: '┌', [DOWN | LEFT]: '┐', [UP | RIGHT]: '└', [UP | LEFT]: '┘',
  [UP | DOWN | RIGHT]: '├', [UP | DOWN | LEFT]: '┤', [DOWN | LEFT | RIGHT]: '┬', [UP | LEFT | RIGHT]: '┴',
  [UP | DOWN | LEFT | RIGHT]: '┼',
};

function lineChar(mask: number, unicode: boolean): string {
  if (unicode) return LINE_UNICODE[mask] ?? ' ';
  if (mask === 0) return ' ';
  const vertical = (mask & (UP | DOWN)) !== 0;
  const horizontal = (mask & (LEFT | RIGHT)) !== 0;
  if (vertical && horizontal) return '+';
  return vertical ? '|' : '-';
}

interface Item {
  id: number; // node id, or a negative id for dummies
  dummy: boolean;
  w: number;
  layer: number;
  order: number;
  x: number;
}

interface Hop {
  upper: Item;
  lower: Item;
  /** The real edge points from the lower item to the upper one (a reversed cycle edge). */
  upward: boolean;
  first: boolean;
  last: boolean;
  hot: boolean;
}

function truncateText(s: string, width: number, unicode: boolean): string {
  if (width <= 0) return '';
  if (s.length <= width) return s;
  return width <= 1 ? s.slice(0, width) : s.slice(0, width - 1) + (unicode ? '…' : '.');
}

export function renderGraph(nodes: GraphNode[], edges: GraphEdge[], opts: GraphOptions): GraphRender {
  const unicode = opts.unicode ?? true;
  const width = Math.max(MIN_W + 2, opts.width);
  const byId = new Map(nodes.map((n) => [n.id, n]));
  const selected = opts.selected ?? null;
  const perNode = opts.maxEdgesPerNode ?? 3;

  // 1. Which edges to draw.
  const valid = edges.filter((e) => e.from !== e.to && byId.has(e.from) && byId.has(e.to));
  const outgoing = new Map<number, GraphEdge[]>();
  for (const e of valid) outgoing.set(e.from, [...(outgoing.get(e.from) ?? []), e]);
  // The selected node's strongest links (in and out) are always drawn.
  const selectedLinks = new Set(
    valid
      .filter((e) => e.from === selected || e.to === selected)
      .sort((a, b) => b.weight - a.weight || a.from - b.from || a.to - b.to)
      .slice(0, 6),
  );
  const drawn: GraphEdge[] = [];
  for (const [, list] of outgoing) {
    const sorted = [...list].sort((a, b) => b.weight - a.weight || a.to - b.to);
    sorted.forEach((e, i) => {
      if (i < perNode || selectedLinks.has(e)) drawn.push(e);
    });
  }
  const hiddenEdges = valid.length - drawn.length;
  // Layers follow the significant links only (at least 5% of a node's
  // outgoing weight, or its strongest one), independent of what is drawn, so
  // boxes keep their places when the selection or link mode changes.
  const outTotal = new Map<number, number>();
  for (const e of valid) outTotal.set(e.from, (outTotal.get(e.from) ?? 0) + e.weight);
  const significant = valid.filter((e) => {
    const strongest = (outgoing.get(e.from) ?? []).reduce((m, x) => Math.max(m, x.weight), 0);
    return e.weight === strongest || e.weight >= 0.05 * (outTotal.get(e.from) ?? 0);
  });

  // 2. Break cycles: DFS from the most upstream nodes. Layers come from all
  //    links, so boxes keep their places whichever links are drawn.
  const weightIn = new Map<number, number>();
  const weightOut = new Map<number, number>();
  for (const e of significant) {
    weightOut.set(e.from, (weightOut.get(e.from) ?? 0) + e.weight);
    weightIn.set(e.to, (weightIn.get(e.to) ?? 0) + e.weight);
  }
  const score = (n: GraphNode) => (n.tone === 'entry' ? 1e12 : 0) + (weightOut.get(n.id) ?? 0) - (weightIn.get(n.id) ?? 0);
  const order = [...nodes].sort((a, b) => score(b) - score(a) || a.id - b.id);
  const out = new Map<number, GraphEdge[]>();
  for (const e of significant) out.set(e.from, [...(out.get(e.from) ?? []), e]);
  for (const [, list] of out) list.sort((a, b) => b.weight - a.weight || a.to - b.to);
  const state = new Map<number, 0 | 1 | 2>(); // 0 new, 1 on stack, 2 done
  const reversed = new Set<GraphEdge>();
  const visit = (id: number) => {
    state.set(id, 1);
    for (const e of out.get(id) ?? []) {
      const s = state.get(e.to) ?? 0;
      if (s === 1) reversed.add(e);
      else if (s === 0) visit(e.to);
    }
    state.set(id, 2);
  };
  for (const n of order) if ((state.get(n.id) ?? 0) === 0) visit(n.id);
  const orient = (e: GraphEdge) => (reversed.has(e) ? { from: e.to, to: e.from, edge: e } : { from: e.from, to: e.to, edge: e });
  const structure = significant.map(orient); // for layering
  const dag = drawn.map(orient); // what is drawn

  // 3. Layers: longest path from the sources; unconnected nodes go to a last row.
  const layer = new Map<number, number>();
  const indeg = new Map<number, number>();
  for (const n of nodes) indeg.set(n.id, 0);
  for (const d of structure) indeg.set(d.to, (indeg.get(d.to) ?? 0) + 1);
  const queue = order.filter((n) => (indeg.get(n.id) ?? 0) === 0).map((n) => n.id);
  for (const id of queue) layer.set(id, 0);
  const succ = new Map<number, number[]>();
  for (const d of structure) succ.set(d.from, [...(succ.get(d.from) ?? []), d.to]);
  for (let i = 0; i < queue.length; i++) {
    const id = queue[i]!;
    for (const t of succ.get(id) ?? []) {
      layer.set(t, Math.max(layer.get(t) ?? 0, (layer.get(id) ?? 0) + 1));
      indeg.set(t, (indeg.get(t) ?? 0) - 1);
      if (indeg.get(t) === 0) queue.push(t);
    }
  }
  const connected = new Set<number>();
  for (const d of [...structure, ...dag]) {
    connected.add(d.from);
    connected.add(d.to);
  }
  let maxLayer = 0;
  for (const n of nodes) if (connected.has(n.id)) maxLayer = Math.max(maxLayer, layer.get(n.id) ?? 0);
  for (const n of nodes) if (!connected.has(n.id)) layer.set(n.id, connected.size > 0 ? maxLayer + 1 : 0);

  // Box widths.
  const boxW = new Map<number, number>();
  for (const n of nodes) boxW.set(n.id, Math.min(MAX_W, width, Math.max(MIN_W, Math.max(n.label.length, n.detail.length) + 4)));

  // 4. Rows that do not fit the width wrap into extra layers (in upstream order).
  let rows: number[][] = [];
  const layerCount = Math.max(0, ...nodes.map((n) => layer.get(n.id) ?? 0)) + 1;
  for (let l = 0; l < layerCount; l++) {
    const members = order.filter((n) => layer.get(n.id) === l).map((n) => n.id);
    let row: number[] = [];
    let used = 0;
    for (const id of members) {
      const w = boxW.get(id)!;
      if (row.length > 0 && used + GAP_X + w > width) {
        rows.push(row);
        row = [];
        used = 0;
      }
      used += (row.length > 0 ? GAP_X : 0) + w;
      row.push(id);
    }
    if (row.length > 0) rows.push(row);
  }
  rows = rows.filter((r) => r.length > 0);
  rows.forEach((r, i) => r.forEach((id) => layer.set(id, i)));

  // 5. Items per layer, with dummies for edges that cross layers.
  const layers: Item[][] = rows.map((r) => r.map((id) => ({ id, dummy: false, w: boxW.get(id)!, layer: 0, order: 0, x: 0 })));
  layers.forEach((items, l) => items.forEach((it) => (it.layer = l)));
  const itemOf = new Map<number, Item>();
  for (const items of layers) for (const it of items) itemOf.set(it.id, it);
  let nextDummy = -1;
  const hops: Hop[] = [];
  for (const d of dag) {
    let a = itemOf.get(d.from)!;
    let b = itemOf.get(d.to)!;
    let upward = reversed.has(d.edge);
    if (a.layer === b.layer) continue; // cannot happen after layering, kept for safety
    if (a.layer > b.layer) {
      [a, b] = [b, a];
      upward = !upward;
    }
    const hot = d.edge.from === selected || d.edge.to === selected;
    let prev = a;
    for (let l = a.layer + 1; l <= b.layer; l++) {
      let cur: Item;
      if (l === b.layer) {
        cur = b;
      } else {
        cur = { id: nextDummy--, dummy: true, w: 1, layer: l, order: 0, x: 0 };
        layers[l]!.push(cur);
      }
      hops.push({ upper: prev, lower: cur, upward, first: prev === a, last: cur === b, hot });
      prev = cur;
    }
  }

  // 6. Order each layer by the barycenter of its neighbors (a few sweeps).
  const up = new Map<Item, Item[]>();
  const down = new Map<Item, Item[]>();
  for (const h of hops) {
    down.set(h.upper, [...(down.get(h.upper) ?? []), h.lower]);
    up.set(h.lower, [...(up.get(h.lower) ?? []), h.upper]);
  }
  layers.forEach((items) => items.forEach((it, i) => (it.order = i)));
  const sweep = (items: Item[], neighbors: Map<Item, Item[]>) => {
    const bary = (it: Item) => {
      const ns = neighbors.get(it) ?? [];
      return ns.length === 0 ? it.order : ns.reduce((s, n) => s + n.order, 0) / ns.length;
    };
    const keyed = items.map((it) => ({ it, k: bary(it) }));
    keyed.sort((p, q) => p.k - q.k || p.it.order - q.it.order);
    keyed.forEach((p, i) => (p.it.order = i));
    items.sort((p, q) => p.order - q.order);
  };
  for (let pass = 0; pass < 4; pass++) {
    for (let l = 1; l < layers.length; l++) sweep(layers[l]!, up);
    for (let l = layers.length - 2; l >= 0; l--) sweep(layers[l]!, down);
  }

  // 7. Horizontal positions. The first row is centered; each later row puts
  //    its items under the average position of their upper neighbors (so long
  //    edges run straight down), then resolves overlaps in row order.
  const gapBefore = (items: Item[], i: number) => (i === 0 ? 0 : items[i]!.dummy || items[i - 1]!.dummy ? 1 : GAP_X);
  layers.forEach((items, l) => {
    const total = items.reduce((sum, it, i) => sum + it.w + gapBefore(items, i), 0);
    let x = Math.max(0, Math.floor((width - total) / 2));
    const packed = items.map((it, i) => {
      x += gapBefore(items, i);
      const at = x;
      x += it.w;
      return at;
    });
    const desired = items.map((it, i) => {
      if (l === 0) return packed[i]!;
      const ups = up.get(it) ?? [];
      if (ups.length === 0) return packed[i]!;
      const mid = ups.reduce((sum, u) => sum + u.x + Math.floor(u.w / 2), 0) / ups.length;
      return Math.round(mid - Math.floor(it.w / 2));
    });
    // Left to right: no overlaps; right to left: stay inside the width.
    items.forEach((it, i) => {
      const min = i === 0 ? 0 : items[i - 1]!.x + items[i - 1]!.w + gapBefore(items, i);
      it.x = Math.max(desired[i]!, min);
    });
    for (let i = items.length - 1; i >= 0; i--) {
      const it = items[i]!;
      const max = i === items.length - 1 ? width - it.w : items[i + 1]!.x - gapBefore(items, i + 1) - it.w;
      it.x = Math.max(0, Math.min(it.x, max));
    }
  });
  const center = (it: Item) => it.x + Math.floor(it.w / 2);

  // 8. Lanes in each gap. Links share a horizontal "bus": the bus of their
  //    source (fan-out) or, when more links converge on the target, the bus of
  //    their target (fan-in); buses that would overlap get separate lanes.
  interface Bus {
    sources: { item: Item; stem: number }[];
    targets: Hop[];
    lo: number;
    hi: number;
    lane: number;
    hot: boolean;
  }
  const stemOf = (it: Item, upward: boolean) => {
    const both = hops.some((o) => o.upper === it && o.upward) && hops.some((o) => o.upper === it && !o.upward);
    const c = center(it);
    return it.dummy || !both ? c : upward ? c + 1 : c - 1;
  };
  const gapBuses: Bus[][] = layers.map(() => []);
  for (let l = 0; l + 1 < layers.length; l++) {
    const inGap = hops.filter((h) => h.upper.layer === l);
    const fanOut = new Map<string, number>();
    const fanIn = new Map<string, number>();
    const srcKey = (h: Hop) => `${h.upper.id}:${h.upward}`;
    const tgtKey = (h: Hop) => `${h.lower.id}:${h.upward}`;
    for (const h of inGap) {
      fanOut.set(srcKey(h), (fanOut.get(srcKey(h)) ?? 0) + 1);
      fanIn.set(tgtKey(h), (fanIn.get(tgtKey(h)) ?? 0) + 1);
    }
    const buses = new Map<string, Bus>();
    for (const h of inGap) {
      const byTarget = (fanIn.get(tgtKey(h)) ?? 0) >= 3 && (fanIn.get(tgtKey(h)) ?? 0) > (fanOut.get(srcKey(h)) ?? 0);
      const key = byTarget ? `t:${tgtKey(h)}` : `s:${srcKey(h)}`;
      let bus = buses.get(key);
      if (!bus) {
        bus = { sources: [], targets: [], lo: Infinity, hi: -Infinity, lane: 0, hot: false };
        buses.set(key, bus);
      }
      const stem = stemOf(h.upper, h.upward);
      if (!bus.sources.some((x) => x.item === h.upper && x.stem === stem)) bus.sources.push({ item: h.upper, stem });
      if (!bus.targets.some((t) => t.lower === h.lower)) bus.targets.push(h);
      for (const x of [stem, center(h.lower)]) {
        bus.lo = Math.min(bus.lo, x);
        bus.hi = Math.max(bus.hi, x);
      }
      bus.hot = bus.hot || h.hot;
    }
    const list = [...buses.values()].sort((p, q) => p.lo - q.lo || p.hi - q.hi);
    const laneEnds: number[] = [];
    for (const bus of list) {
      if (bus.lo === bus.hi) {
        bus.lane = -1; // straight down: no lane needed
        continue;
      }
      let lane = laneEnds.findIndex((end) => end < bus.lo - 1);
      if (lane < 0) {
        lane = laneEnds.length;
        laneEnds.push(bus.hi);
      } else {
        laneEnds[lane] = bus.hi;
      }
      bus.lane = lane;
    }
    gapBuses[l] = list;
  }
  const gapHeight = gapBuses.map((buses, l) =>
    l + 1 < layers.length ? Math.max(0, ...buses.map((bus) => bus.lane + 1)) + 2 : 0,
  );

  // 9. Vertical positions.
  const layerY: number[] = [];
  let y = 0;
  layers.forEach((_, l) => {
    layerY.push(y);
    y += BOX_H + gapHeight[l]!;
  });
  const height = Math.max(0, y);

  // 10. Draw.
  const mask: number[][] = Array.from({ length: height }, () => new Array(width).fill(0));
  const hot: boolean[][] = Array.from({ length: height }, () => new Array(width).fill(false));
  const chars: (string | null)[][] = Array.from({ length: height }, () => new Array(width).fill(null));
  const styles: CellStyle[][] = Array.from({ length: height }, () => new Array(width).fill('none'));
  const inside = (r: number, c: number) => r >= 0 && r < height && c >= 0 && c < width;
  const link = (r: number, c: number, bits: number, isHot: boolean) => {
    if (!inside(r, c)) return;
    mask[r]![c]! |= bits;
    if (isHot) hot[r]![c] = true;
  };
  const vline = (c: number, r1: number, r2: number, isHot: boolean) => {
    for (let r = Math.min(r1, r2); r <= Math.max(r1, r2); r++) {
      link(r, c, (r > Math.min(r1, r2) ? UP : 0) | (r < Math.max(r1, r2) ? DOWN : 0), isHot);
    }
  };
  const hline = (r: number, c1: number, c2: number, isHot: boolean) => {
    for (let c = Math.min(c1, c2); c <= Math.max(c1, c2); c++) {
      link(r, c, (c > Math.min(c1, c2) ? LEFT : 0) | (c < Math.max(c1, c2) ? RIGHT : 0), isHot);
    }
  };
  const put = (r: number, c: number, ch: string, style: CellStyle) => {
    if (!inside(r, c)) return;
    chars[r]![c] = ch;
    styles[r]![c] = style;
  };

  // Dummies: a vertical line through the box rows of their layer.
  for (const items of layers) {
    for (const it of items) {
      if (!it.dummy) continue;
      const hotLine = hops.some((h) => (h.upper === it || h.lower === it) && h.hot);
      vline(center(it), layerY[it.layer]!, layerY[it.layer]! + BOX_H - 1, hotLine);
      link(layerY[it.layer]!, center(it), UP, hotLine);
      link(layerY[it.layer]! + BOX_H - 1, center(it), DOWN, hotLine);
    }
  }
  const arrows: { r: number; c: number; ch: string; hot: boolean }[] = [];
  const stemsOut: { r: number; c: number }[] = [];
  const stemsIn: { r: number; c: number }[] = [];
  gapBuses.forEach((buses, l) => {
    if (l + 1 >= layers.length) return;
    const top = layerY[l]! + BOX_H; // first gap row
    const bottom = layerY[l + 1]! - 1; // last gap row, just above the lower layer
    for (const bus of buses) {
      const laneRow = bus.lane < 0 ? bottom : top + 1 + bus.lane;
      for (const src of bus.sources) {
        vline(src.stem, top, laneRow, bus.hot);
        link(top, src.stem, UP, bus.hot); // joins the box border or the dummy above
        if (!src.item.dummy) stemsOut.push({ r: top - 1, c: src.stem });
        const upwardFirst = bus.targets.some((h) => h.upper === src.item && h.upward && h.first);
        if (upwardFirst && !src.item.dummy) arrows.push({ r: top, c: src.stem, ch: unicode ? '▲' : '^', hot: bus.hot });
      }
      if (bus.lane >= 0) hline(laneRow, bus.lo, bus.hi, bus.hot);
      for (const h of bus.targets) {
        const tx = center(h.lower);
        vline(tx, laneRow, bottom, bus.hot);
        link(bottom, tx, DOWN, bus.hot);
        if (h.lower.dummy) continue;
        if (!h.upward && h.last) arrows.push({ r: bottom, c: tx, ch: unicode ? '▼' : 'v', hot: bus.hot });
        else stemsIn.push({ r: bottom + 1, c: tx });
      }
    }
  });
  for (let r = 0; r < height; r++) {
    for (let c = 0; c < width; c++) {
      if (mask[r]![c]) put(r, c, lineChar(mask[r]![c]!, unicode), hot[r]![c] ? 'edgeHot' : 'edge');
    }
  }
  for (const a of arrows) put(a.r, a.c, a.ch, a.hot ? 'arrowHot' : 'arrow');

  // Boxes on top.
  const boxes: BoxPos[] = [];
  for (const items of layers) {
    for (const it of items) {
      if (it.dummy) continue;
      const n = byId.get(it.id)!;
      const r0 = layerY[it.layer]!;
      const sel = it.id === selected;
      const border: CellStyle = sel ? 'boxSel' : n.tone === 'outside' || n.tone === 'other' ? 'dim' : 'box';
      const h = unicode ? (sel ? '━' : '─') : sel ? '=' : '-';
      const v = unicode ? (sel ? '┃' : '│') : sel ? '#' : '|';
      const corners = unicode ? (sel ? ['┏', '┓', '┗', '┛'] : ['┌', '┐', '└', '┘']) : ['+', '+', '+', '+'];
      for (let c = it.x; c < it.x + it.w; c++) {
        put(r0, c, h, border);
        put(r0 + BOX_H - 1, c, h, border);
      }
      for (let r = r0 + 1; r < r0 + BOX_H - 1; r++) {
        put(r, it.x, v, border);
        put(r, it.x + it.w - 1, v, border);
        for (let c = it.x + 1; c < it.x + it.w - 1; c++) put(r, c, ' ', 'none');
      }
      put(r0, it.x, corners[0]!, border);
      put(r0, it.x + it.w - 1, corners[1]!, border);
      put(r0 + BOX_H - 1, it.x, corners[2]!, border);
      put(r0 + BOX_H - 1, it.x + it.w - 1, corners[3]!, border);
      const inner = it.w - 4;
      const label = truncateText(n.label, inner, unicode);
      const detail = truncateText(n.detail, inner, unicode);
      const labelStyle: CellStyle = sel ? 'labelSel' : n.tone === 'entry' ? 'entry' : n.tone === 'outside' || n.tone === 'other' ? 'dim' : 'label';
      [...label].forEach((ch, i) => put(r0 + 1, it.x + 2 + i, ch, labelStyle));
      [...detail].forEach((ch, i) => put(r0 + 2, it.x + 2 + i, ch, 'detail'));
      boxes.push({ id: it.id, x: it.x, y: r0, w: it.w, h: BOX_H, layer: it.layer, order: it.order });
    }
  }
  // Where lines meet a box border.
  for (const s of stemsOut) {
    if (inside(s.r, s.c) && styles[s.r]![s.c] !== 'none') put(s.r, s.c, unicode ? '┬' : '+', styles[s.r]![s.c]!);
  }
  for (const s of stemsIn) {
    if (inside(s.r, s.c) && styles[s.r]![s.c] !== 'none') put(s.r, s.c, unicode ? '┴' : '+', styles[s.r]![s.c]!);
  }

  // Segments per line.
  const lines: Segment[][] = [];
  for (let r = 0; r < height; r++) {
    const segs: Segment[] = [];
    for (let c = 0; c < width; c++) {
      const ch = chars[r]![c] ?? ' ';
      const st = chars[r]![c] === null ? 'none' : styles[r]![c]!;
      const last = segs[segs.length - 1];
      if (last && last.style === st) last.text += ch;
      else segs.push({ text: ch, style: st });
    }
    // Trailing blanks are not needed.
    const tail = segs[segs.length - 1];
    if (tail && tail.style === 'none') tail.text = tail.text.replace(/\s+$/, '');
    lines.push(segs.filter((s) => s.text.length > 0));
  }
  boxes.sort((a, b) => a.layer - b.layer || a.x - b.x);
  return { width, height, lines, boxes, hiddenEdges };
}

/** The box reached from `current` by an arrow key (left/right within a row, up/down to the nearest box). */
export function neighborBox(boxes: BoxPos[], current: number | null, dir: 'left' | 'right' | 'up' | 'down'): number | null {
  if (boxes.length === 0) return null;
  const cur = boxes.find((b) => b.id === current);
  if (!cur) return boxes[0]!.id;
  const mid = (b: BoxPos) => b.x + b.w / 2;
  if (dir === 'left' || dir === 'right') {
    const row = boxes.filter((b) => b.layer === cur.layer).sort((a, b) => a.x - b.x);
    const i = row.findIndex((b) => b.id === cur.id) + (dir === 'right' ? 1 : -1);
    return i >= 0 && i < row.length ? row[i]!.id : cur.id;
  }
  const layers = [...new Set(boxes.map((b) => b.layer))].sort((a, b) => a - b);
  const li = layers.indexOf(cur.layer) + (dir === 'down' ? 1 : -1);
  if (li < 0 || li >= layers.length) return cur.id;
  const row = boxes.filter((b) => b.layer === layers[li]);
  row.sort((a, b) => Math.abs(mid(a) - mid(cur)) - Math.abs(mid(b) - mid(cur)) || a.x - b.x);
  return row[0]!.id;
}

/** Plain text of a rendering (tests, non-interactive output). */
export function graphText(g: GraphRender): string {
  return g.lines.map((segs) => segs.map((s) => s.text).join('')).join('\n');
}

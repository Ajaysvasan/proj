// The code map renderer: layered layout, boxes, arrows and navigation.
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import { type GraphEdge, type GraphNode, graphText, neighborBox, renderGraph } from '../src/ui/graph.js';

const nodes: GraphNode[] = [
  { id: 0, label: 'config', detail: '3 files · 12 routes', tone: 'entry' },
  { id: 1, label: 'web', detail: '13 files' },
  { id: 2, label: 'service', detail: '3 files' },
  { id: 3, label: 'dao', detail: '2 files' },
  { id: 4, label: 'docs', detail: '1 file' },
];
const edges: GraphEdge[] = [
  { from: 0, to: 1, weight: 15 },
  { from: 1, to: 2, weight: 14 },
  { from: 2, to: 3, weight: 3 },
  { from: 0, to: 3, weight: 2 }, // spans two layers
  { from: 3, to: 2, weight: 1 }, // a cycle
];

function overlaps(a: { x: number; y: number; w: number; h: number }, b: { x: number; y: number; w: number; h: number }) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

describe('code map rendering', () => {
  it('draws every node as a box, layered from entry points down, within the width', () => {
    const g = renderGraph(nodes, edges, { width: 80, maxEdgesPerNode: 3 });
    assert.equal(g.boxes.length, nodes.length);
    for (const b of g.boxes) assert.ok(b.x >= 0 && b.x + b.w <= 80, `box ${b.id} fits`);
    for (let i = 0; i < g.boxes.length; i++) {
      for (let j = i + 1; j < g.boxes.length; j++) assert.ok(!overlaps(g.boxes[i]!, g.boxes[j]!), 'boxes do not overlap');
    }
    const layerOf = (id: number) => g.boxes.find((b) => b.id === id)!.layer;
    assert.ok(layerOf(0) < layerOf(1) && layerOf(1) < layerOf(2) && layerOf(2) < layerOf(3), 'flow goes downward');
    assert.ok(layerOf(4) > layerOf(3), 'unconnected parts sit below the graph');
    const text = graphText(g);
    for (const n of nodes) assert.match(text, new RegExp(n.label));
    assert.ok((text.match(/▼/g) ?? []).length >= 3, 'arrowheads point at targets');
    assert.match(text, /▲/, 'the cycle edge points back up');
    for (const line of text.split('\n')) assert.ok([...line].length <= 80);
  });

  it('keeps the strongest link per part by default and all links of the selected part', () => {
    const backbone = renderGraph(nodes, edges, { width: 80, maxEdgesPerNode: 1 });
    assert.equal(backbone.hiddenEdges, 1); // 0 -> 3 is weaker than 0 -> 1
    const focused = renderGraph(nodes, edges, { width: 80, maxEdgesPerNode: 1, selected: 3 });
    assert.equal(focused.hiddenEdges, 0);
    const hot = focused.lines.flat().filter((s) => s.style === 'edgeHot' || s.style === 'arrowHot');
    assert.ok(hot.length > 0, 'links of the selected part are highlighted');
    assert.ok(focused.lines.flat().some((s) => s.style === 'boxSel'));
  });

  it('wraps parts that do not fit the width into more rows', () => {
    const many: GraphNode[] = Array.from({ length: 9 }, (_, i) => ({ id: i, label: `module-${i}`, detail: '10 files' }));
    const g = renderGraph(many, [], { width: 60 });
    assert.equal(g.boxes.length, 9);
    assert.ok(new Set(g.boxes.map((b) => b.layer)).size >= 3);
    for (const b of g.boxes) assert.ok(b.x + b.w <= 60);
  });

  it('falls back to ASCII', () => {
    const plain = nodes.map((n) => ({ ...n, detail: n.detail.replace('·', '-') }));
    const text = graphText(renderGraph(plain, edges, { width: 80, unicode: false, selected: 1 }));
    assert.ok(/^[\x20-\x7e\n]*$/.test(text), 'only ASCII characters');
    assert.match(text, /v/);
  });

  it('moves the selection with arrow keys', () => {
    const g = renderGraph(nodes, edges, { width: 80 });
    assert.equal(neighborBox(g.boxes, 0, 'down'), 1);
    assert.equal(neighborBox(g.boxes, 1, 'up'), 0);
    assert.equal(neighborBox(g.boxes, 0, 'up'), 0, 'stays at the top');
    assert.equal(neighborBox(g.boxes, null, 'down'), g.boxes[0]!.id);
  });
});

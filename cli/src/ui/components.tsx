import { Box, Text } from 'ink';
import React from 'react';

import { glyph, truncate } from '../format.js';
import { describeRow, layoutTree, type TraverseNode, type TreeRow } from './tree.js';

export function clamp(n: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(n, hi));
}

/** Windowed list that keeps the selected row visible. */
export function SelectList<T>(props: {
  items: T[];
  selected: number;
  height: number;
  width: number;
  render: (item: T) => string;
  color?: (item: T) => string | undefined;
  empty?: string;
}): React.ReactElement {
  const { items, selected, width } = props;
  const height = Math.max(1, props.height);
  if (items.length === 0) return <Text dimColor>{props.empty ?? '(nothing to show)'}</Text>;
  const start = clamp(selected - Math.floor(height / 2), 0, Math.max(0, items.length - height));
  const visible = items.slice(start, start + height);
  return (
    <Box flexDirection="column">
      {visible.map((item, i) => {
        const index = start + i;
        const isSel = index === selected;
        const text = truncate(`${isSel ? glyph.sel : ' '} ${props.render(item)}`, width);
        return (
          <Text key={index} inverse={isSel} color={isSel ? undefined : props.color?.(item)} wrap="truncate">
            {text}
          </Text>
        );
      })}
      {items.length > height ? (
        <Text dimColor>
          {`  ${start + 1}-${Math.min(start + height, items.length)} of ${items.length}`}
        </Text>
      ) : null}
    </Box>
  );
}

export function Tabs(props: { tabs: string[]; active: number; right?: string }): React.ReactElement {
  return (
    <Box>
      {props.tabs.map((t, i) => (
        <Text key={t} bold={i === props.active} inverse={i === props.active}>
          {` ${t} `}
        </Text>
      ))}
      {props.right ? <Text dimColor>{`  ${props.right}`}</Text> : null}
    </Box>
  );
}

export function statusColor(status: string | undefined): string | undefined {
  switch (status) {
    case 'unresolved':
      return 'red';
    case 'ambiguous':
    case 'heuristic':
      return 'yellow';
    case 'external':
      return 'cyan';
    default:
      return undefined;
  }
}

/** Selectable flow tree built from a traverse result. */
export function FlowTree(props: {
  nodes: TraverseNode[];
  selected: number;
  height: number;
  width: number;
}): React.ReactElement {
  const rows = layoutTree(props.nodes);
  return (
    <SelectList<TreeRow>
      items={rows}
      selected={props.selected}
      height={props.height}
      width={props.width}
      render={(r) => r.prefix + describeRow(r)}
      color={(r) => (r.node.seen ? 'gray' : statusColor(r.node.edge?.status))}
      empty="(no relationships)"
    />
  );
}

export function Loading(props: { what: string }): React.ReactElement {
  return <Text dimColor>{`Loading ${props.what}...`}</Text>;
}

export function ErrorLine(props: { message: string }): React.ReactElement {
  return <Text color="red">{`Error: ${props.message}`}</Text>;
}

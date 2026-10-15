// Runtime tracing screens: the Error Tracer (observed exceptions) and observed traces, both placed
// in the static index where their methods could be mapped.
import { Box, Text, useInput } from 'ink';
import { useState } from 'react';

import { formatCount, glyph, truncate } from '../format.js';
import { clamp, ErrorLine, Loading, SelectList } from './components.js';
import { useLatest, useQuery } from './hooks.js';
import { durationText, errorLines, shortClass, traceLabel, traceLines } from './runtime_text.js';
import type { ViewProps } from './views.js';

function useListKeys(props: ViewProps, count: number, onEnter: (i: number) => void) {
  const [selected, setSelected] = useState(0);
  const latest = useLatest({ count, selected, onEnter });
  useInput(
    (_i, key) => {
      const { count, selected, onEnter } = latest.current;
      if (key.upArrow) setSelected((s) => clamp(s - 1, 0, Math.max(0, count - 1)));
      if (key.downArrow) setSelected((s) => clamp(s + 1, 0, Math.max(0, count - 1)));
      if (key.pageUp) setSelected((s) => clamp(s - 10, 0, Math.max(0, count - 1)));
      if (key.pageDown) setSelected((s) => clamp(s + 10, 0, Math.max(0, count - 1)));
      if (key.return && count > 0) onEnter(selected);
    },
    { isActive: props.active },
  );
  return selected;
}

const NOTHING_YET =
  'Nothing observed yet. Run `proj start dynamic <project>` and start the JVM with the -javaagent option it prints.';

export function ErrorsView(props: ViewProps) {
  const res = useQuery<any>(props.session, 'runtime_errors', { project: props.project, limit: 500 }, 'runtime_errors');
  const items: any[] = res.data?.errors ?? [];
  const selected = useListKeys(props, items.length, (i) => props.open({ kind: 'error', id: items[i].id }));
  if (res.loading) return <Loading what="observed errors" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <Text bold>{`Error Tracer: exceptions observed at runtime (${formatCount(res.data?.total ?? 0)})`}</Text>
      <SelectList
        items={items}
        selected={selected}
        height={props.height - 2}
        width={props.width}
        render={(e) => {
          const top = e.top_app_frame ? ` at ${shortClass(e.top_app_frame.class)}.${e.top_app_frame.method}:${e.top_app_frame.line}` : '';
          return `${e.state.padEnd(8)} ${shortClass(e.exception)}${e.message ? `: ${e.message}` : ''}${top}  ${glyph.times}${e.occurrences}`;
        }}
        color={(e) => (e.state === 'UNCAUGHT' || e.state === 'ESCAPED' ? 'red' : e.state === 'UNKNOWN' ? 'yellow' : undefined)}
        empty={res.data?.store === false ? NOTHING_YET : 'No exceptions were observed.'}
      />
    </Box>
  );
}

export function ErrorView(props: ViewProps & { id: number }) {
  const res = useQuery<any>(props.session, 'runtime_error', { project: props.project, error_id: props.id }, `runtime_error:${props.id}`);
  const lines = res.data ? errorLines(res.data) : [];
  const selected = useListKeys(props, lines.length, (i) => {
    const s = lines[i]?.symbol;
    if (s !== undefined) props.open({ kind: 'symbol', id: s });
  });
  if (res.loading) return <Loading what="error" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <SelectList items={lines} selected={selected} height={props.height - 1} width={props.width} render={(l) => l.text} color={(l) => l.color} />
      <Text dimColor>{truncate('Enter on a frame opens its method; frames in gray are outside the project', props.width)}</Text>
    </Box>
  );
}

export function TracesView(props: ViewProps) {
  const res = useQuery<any>(props.session, 'runtime_traces', { project: props.project, limit: 500 }, 'runtime_traces');
  const items: any[] = res.data?.traces ?? [];
  const selected = useListKeys(props, items.length, (i) => props.open({ kind: 'trace', id: items[i].id }));
  if (res.loading) return <Loading what="observed traces" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <Text bold>{`Observed traces, newest first (${formatCount(res.data?.total ?? 0)})`}</Text>
      <SelectList
        items={items}
        selected={selected}
        height={props.height - 2}
        width={props.width}
        render={(t) =>
          `${traceLabel(t)}  [${t.thread}]  ${t.nodes} methods  ${durationText(t.duration_us)}${t.errors > 0 ? `  ${t.errors} error(s)` : ''}${t.status !== 'complete' ? `  [${t.status}]` : ''}`
        }
        color={(t) => (t.errors > 0 ? 'red' : t.status !== 'complete' ? 'yellow' : undefined)}
        empty={res.data?.store === false ? NOTHING_YET : 'No traces were recorded.'}
      />
    </Box>
  );
}

export function TraceView(props: ViewProps & { id: number }) {
  const res = useQuery<any>(props.session, 'runtime_trace', { project: props.project, trace_id: props.id }, `runtime_trace:${props.id}`);
  const lines = res.data ? traceLines(res.data) : [];
  const selected = useListKeys(props, lines.length, (i) => {
    const l = lines[i];
    if (l?.symbol !== undefined) props.open({ kind: 'symbol', id: l.symbol });
    else {
      const m = /^error #(\d+)/.exec(l?.text ?? '');
      if (m) props.open({ kind: 'error', id: Number(m[1]) });
    }
  });
  if (res.loading) return <Loading what="trace" />;
  if (res.error) return <ErrorLine message={res.error} />;
  return (
    <Box flexDirection="column">
      <SelectList items={lines} selected={selected} height={props.height - 1} width={props.width} render={(l) => l.text} color={(l) => l.color} />
      <Text dimColor>{truncate('Calls in order of first entry; [runtime only]: no static call site, [not in the index]: code proj did not analyze', props.width)}</Text>
    </Box>
  );
}

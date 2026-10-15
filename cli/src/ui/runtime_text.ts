// Text for runtime observations (no UI dependencies): shared by the explorer screens and by
// `proj errors` / `proj traces`.
import { glyph } from '../format.js';

/** What an error state means, in one line. */
export const STATE_TEXT: Record<string, string> = {
  CAUGHT: 'caught by application code',
  WRAPPED: 'caught and wrapped in another exception',
  ESCAPED: 'left the outermost application method (the framework handled what happened next)',
  UNCAUGHT: 'terminated its thread',
  UNKNOWN: 'its handling could not be established',
};

export function shortClass(name: string): string {
  return name.slice(name.lastIndexOf('.') + 1);
}

export function frameText(f: any): string {
  const where = f.file ? `${f.file}:${f.line}` : f.line > 0 ? `line ${f.line}` : 'unknown source';
  return `${shortClass(f.class)}.${f.method}()  ${where}`;
}

export function durationText(us: number): string {
  if (us < 1000) return `${us} us`;
  if (us < 1_000_000) return `${(us / 1000).toFixed(1)} ms`;
  return `${(us / 1_000_000).toFixed(2)} s`;
}

export function traceLabel(t: any): string {
  const what = t.http ? `${t.http.method} ${t.http.path}` : t.root?.display ?? '?';
  return t.http ? `${what}  ${t.root?.display ?? ''}` : what;
}

export interface Line {
  text: string;
  symbol?: number;
  bold?: boolean;
  color?: string;
}

export function errorLines(e: any): Line[] {
  const lines: Line[] = [];
  lines.push({ text: `${e.exception}${e.message ? `: ${e.message}` : ''}`, bold: true });
  lines.push({ text: `${e.state}: ${STATE_TEXT[e.state] ?? ''}${e.rethrown ? '; rethrown after a catch' : ''}${e.wrapped_by ? `; wrapped by ${e.wrapped_by}` : ''}  (${glyph.times}${e.occurrences}, last ${e.last_seen})` });
  if (e.throw_site) {
    lines.push({ text: `thrown at  ${e.throw_site.method.display}  line ${e.throw_site.line}`, symbol: e.throw_site.symbol?.id });
  } else {
    lines.push({ text: 'thrown outside application code (a library or the JDK); the stack below shows where it entered', color: 'gray' });
  }
  if (e.caught_at) lines.push({ text: `caught at  ${e.caught_at.method.display}  line ${e.caught_at.line}`, symbol: e.caught_at.symbol?.id });
  if (e.escaped_from) lines.push({ text: `escaped from  ${e.escaped_from.display}` });
  lines.push({ text: 'Stack (most recent call first):', bold: true });
  e.frames.forEach((f: any, i: number) => {
    const mapped = f.symbol ? `  ${glyph.arrow} ${f.symbol.path}:${f.line}` : f.application ? '  [not in the index]' : '';
    lines.push({ text: `${String(i + 1).padStart(3)}. ${frameText(f)}${mapped}`, symbol: f.symbol?.id, color: f.application ? undefined : 'gray' });
  });
  if (e.frames_omitted > 0) lines.push({ text: `     ... ${e.frames_omitted} more frames`, color: 'gray' });
  for (const c of e.causes ?? []) {
    lines.push({ text: `Caused by ${c.exception}${c.message ? `: ${c.message}` : ''}`, bold: true });
    c.frames.forEach((f: any, i: number) => {
      lines.push({ text: `${String(i + 1).padStart(3)}. ${frameText(f)}`, symbol: f.symbol?.id, color: f.application ? undefined : 'gray' });
    });
  }
  if ((e.events ?? []).length > 0) {
    lines.push({ text: 'What the application code saw:', bold: true });
    for (const ev of e.events) lines.push({ text: `  ${ev.kind.padEnd(8)} ${ev.method.display}${ev.line ? ` line ${ev.line}` : ''}`, color: 'gray' });
  }
  if ((e.traces ?? []).length > 0) lines.push({ text: `Traces: ${e.traces.map((t: any) => `#${t.id}${t.http ? ` ${t.http.method} ${t.http.path}` : ''}`).join(', ')}` });
  return lines;
}

const EVIDENCE_TEXT: Record<string, string> = {
  STATIC_CONFIRMED: '',
  STATIC_INFERRED: ' [static: inferred]',
  RUNTIME_OBSERVED: ' [runtime only]',
  RUNTIME_UNRESOLVED: ' [not in the index]',
};

export function traceLines(t: any): Line[] {
  const lines: Line[] = [];
  const coverage = t.startup_coverage === 'complete' ? 'recorded since JVM startup' : t.startup_coverage === 'attached_late' ? 'agent attached to a running JVM' : 'recording was interrupted';
  lines.push({ text: `${t.http ? `${t.http.method} ${t.http.path}  ` : ''}thread ${t.thread}, pid ${t.pid}, ${durationText(t.duration_us)}, ${t.status}; ${coverage}`, bold: true });
  if (t.dropped_nodes > 0) lines.push({ text: `${t.dropped_nodes} further calls were not recorded (node limit)`, color: 'yellow' });
  for (const n of t.nodes) {
    const sym = n.symbol ? `  ${glyph.arrow} ${n.symbol.path}:${n.symbol.line}` : '';
    const calls = n.calls > 1 ? ` ${glyph.times}${n.calls}` : '';
    const exc = n.exits_by_exception > 0 ? ' [exception]' : '';
    const ev = n.evidence ? EVIDENCE_TEXT[n.evidence] ?? '' : '';
    lines.push({
      text: `${'  '.repeat(n.depth)}${n.method.display}${calls}${exc}${ev}${sym}`,
      symbol: n.symbol?.id,
      color: n.evidence === 'RUNTIME_UNRESOLVED' ? 'gray' : n.evidence === 'RUNTIME_OBSERVED' ? 'magenta' : n.exits_by_exception > 0 ? 'red' : undefined,
    });
  }
  for (const e of t.errors ?? []) lines.push({ text: `error #${e.id}: ${e.state} ${e.exception}${e.message ? `: ${e.message}` : ''}`, color: 'red' });
  return lines;
}


function sourceOf(symbol: any, line?: number): string {
  return symbol ? `  ${glyph.arrow} ${symbol.path}${line ? `:${line}` : ''}` : '';
}

/**
 * `proj errors <project> <id>`: frames from the most recent (deepest) call to the oldest, as the
 * JVM recorded them, each with its location on the next line. The throw site, the most recent
 * application frame and the handler are reported separately: they are often, not always, the same.
 */
export function errorReport(e: any): string[] {
  const out: string[] = [];
  out.push(`Exception: ${e.exception}${e.message ? `: ${e.message}` : ''}`);
  out.push(`State: ${e.state}, ${STATE_TEXT[e.state] ?? 'unknown'}${e.rethrown ? '; rethrown after a catch' : ''}${e.wrapped_by ? `; wrapped by ${e.wrapped_by}` : ''}`);
  out.push(`Seen ${e.occurrences} time(s), first ${e.first_seen}, last ${e.last_seen}${e.thread ? `, thread ${e.thread}` : ''}`);
  out.push(
    e.throw_site
      ? `Thrown at: ${e.throw_site.method.display} line ${e.throw_site.line}${sourceOf(e.throw_site.symbol, e.throw_site.line)}`
      : 'Thrown at: outside application code (a library or the JDK); not observed directly',
  );
  if (e.top_app_frame) {
    const f = e.top_app_frame;
    out.push(`Most recent application frame: ${shortClass(f.class)}.${f.method}() line ${f.line}${sourceOf(f.symbol, f.line)}`);
  }
  if (e.caught_at) {
    out.push(`Caught at: ${e.caught_at.method.display} line ${e.caught_at.line}${e.caught_type ? ` (catch ${e.caught_type})` : ''}${sourceOf(e.caught_at.symbol, e.caught_at.line)}`);
  }
  if (e.escaped_from) out.push(`Escaped from: ${e.escaped_from.display} (the outermost application method of its trace)`);
  const frames = (list: any[], omitted: number) => {
    list.forEach((f: any, i: number) => {
      out.push(`${i + 1}. ${shortClass(f.class)}.${f.method}()${f.application ? '' : '  [outside the project]'}`);
      const where = f.file ? `${f.file}:${f.line}` : 'unknown source';
      out.push(`   ${where}${f.symbol ? sourceOf(f.symbol) : f.application ? '  (not in the index)' : ''}`);
    });
    if (omitted > 0) out.push(`   ... ${omitted} more frames`);
  };
  out.push('');
  out.push('Stack, most recent call first:');
  frames(e.frames, e.frames_omitted);
  for (const c of e.causes ?? []) {
    out.push('');
    out.push(`Caused by: ${c.exception}${c.message ? `: ${c.message}` : ''}`);
    frames(c.frames, c.frames_omitted);
  }
  if (e.suppressed > 0) out.push(`(${e.suppressed} suppressed exception(s) not shown)`);
  if ((e.events ?? []).length > 0) {
    out.push('');
    out.push('Observed in application code:');
    for (const ev of e.events) out.push(`  ${ev.kind.padEnd(8)} ${ev.method.display}${ev.line ? ` line ${ev.line}` : ''}`);
  }
  if ((e.traces ?? []).length > 0) {
    out.push('');
    out.push(`Traces: ${e.traces.map((t: any) => `#${t.id}${t.http ? ` ${t.http.method} ${t.http.path}` : ''}`).join(', ')}`);
  }
  return out;
}

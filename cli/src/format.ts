import chalk from 'chalk';

// Terminals that cannot render Unicode get ASCII glyphs (TERM=dumb, non-UTF-8 locale or PROJ_ASCII=1).
function unicodeSupported(env: NodeJS.ProcessEnv = process.env): boolean {
  if (env.PROJ_ASCII === '1') return false;
  if (env.TERM === 'dumb') return false;
  const locale = env.LC_ALL || env.LC_CTYPE || env.LANG || '';
  return locale === '' || /utf-?8/i.test(locale);
}

export const UNICODE = unicodeSupported();

export const glyph = UNICODE
  ? { branch: '├─ ', last: '└─ ', pipe: '│  ', space: '   ', arrow: '→', back: '←', seen: '↺', ext: '↗', bullet: '•', sel: '›', times: '×' }
  : { branch: '|- ', last: '`- ', pipe: '|  ', space: '   ', arrow: '->', back: '<-', seen: '@', ext: '^', bullet: '*', sel: '>', times: 'x' };

export function formatDuration(ms: number | null | undefined): string {
  if (ms === null || ms === undefined || Number.isNaN(ms)) return '-';
  if (ms < 1000) return `${Math.round(ms)} ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(2)} s`;
  const m = Math.floor(ms / 60000);
  const s = Math.round((ms % 60000) / 1000);
  return `${m}m ${s}s`;
}

export function formatCount(n: number | null | undefined): string {
  if (n === null || n === undefined) return '-';
  return n.toLocaleString('en-US');
}

export function formatBytes(n: number | null | undefined): string {
  if (n === null || n === undefined) return '-';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MiB`;
}

export function colorStatus(status: string): string {
  switch (status) {
    case 'success':
    case 'ok':
    case 'resolved':
    case 'parsed':
      return chalk.green(status);
    case 'partial':
    case 'heuristic':
    case 'ambiguous':
    case 'syntax_errors':
    case 'no_supported_files':
    case 'interrupted':
    case 'warn':
      return chalk.yellow(status);
    case 'failed':
    case 'unresolved':
    case 'error':
    case 'parse_failed':
      return chalk.red(status);
    case 'external':
    case 'running':
      return chalk.cyan(status);
    default:
      return status;
  }
}

/** One-character marker for an edge's resolution status. */
export function statusMarker(status: string): string {
  switch (status) {
    case 'resolved':
      return ' ';
    case 'heuristic':
      return '~';
    case 'ambiguous':
      return '?';
    case 'external':
      return glyph.ext;
    case 'unresolved':
      return '!';
    default:
      return ' ';
  }
}

export function truncate(s: string, width: number): string {
  if (width <= 1) return '';
  return s.length <= width ? s : s.slice(0, width - 1) + (UNICODE ? '…' : '~');
}

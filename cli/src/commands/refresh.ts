import chalk from 'chalk';

import { type Context, engineOptions, processError, responseError } from '../context.js';
import { runEngine } from '../engine/client.js';
import { reportAnalysis } from './add.js';

export interface RefreshOptions {
  json?: boolean;
}

/**
 * Re-indexes only what changed since the last analysis: files are compared by
 * content fingerprint, changed and added files are re-parsed, facts of removed
 * files are deleted, and affected relationships are resolved again.
 */
export async function refreshCommand(project: string, opts: RefreshOptions, ctx: Context): Promise<number> {
  if (!ctx.quiet && !opts.json) process.stderr.write(chalk.dim(`Refreshing ${project}...\n`));
  let res;
  try {
    res = await runEngine('refresh_project', { project }, engineOptions(ctx));
  } catch (err) {
    throw processError(ctx, 'refresh_project', err);
  }
  if (res.status !== 'ok' && res.status !== 'partial') {
    const err = responseError(ctx, 'refresh_project', res);
    const details = res.error?.details as { previous_index_kept?: boolean; run_id?: number } | undefined;
    if (res.status === 'analysis_failed' && details) {
      if (details.previous_index_kept) err.message += '\nThe previous index was kept unchanged.';
      if (details.run_id) err.message += `\nDetails: proj log ${project} --run ${details.run_id}`;
    }
    throw err;
  }
  return reportAnalysis(res, opts.json ?? false, ctx);
}

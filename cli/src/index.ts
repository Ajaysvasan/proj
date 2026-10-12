#!/usr/bin/env node
// proj: local static code-flow explorer. See docs/CLI_REFERENCE.md.
import chalk from 'chalk';
import { Command, CommanderError, Option } from 'commander';

import { appendAppLog } from './applog.js';
import { addCommand } from './commands/add.js';
import { logCommand } from './commands/log.js';
import { refreshCommand } from './commands/refresh.js';
import { showCommand } from './commands/show.js';
import { CliError, type GlobalOptions, makeContext } from './context.js';
import { ExitCode } from './exit-codes.js';
import { resolveDataDir } from './paths.js';
import { CLI_VERSION } from './version.js';

function collect(value: string, previous: string[] = []): string[] {
  return [...previous, value];
}

export function buildProgram(run: (code: number) => void): Command {
  const program = new Command();
  program
    .name('proj')
    .description('Local, static code-flow explorer: index a project, then search symbols, routes and call flows.')
    .version(CLI_VERSION, '-V, --version')
    .option('--data-dir <dir>', 'where the registry, indexes and logs live (default: $PROJ_HOME or the platform data directory)')
    .option('--engine <path>', 'path to the proj-engine binary (default: $PROJ_ENGINE or the repository build)')
    .option('-v, --verbose', 'stream engine progress and diagnostics to stderr')
    .option('--debug', 'like --verbose, with debug-level engine logging')
    .option('-q, --quiet', 'print nothing on success; rely on the exit code')
    .showHelpAfterError('(run "proj --help" for usage)')
    .exitOverride()
    .configureOutput({ writeErr: (s) => process.stderr.write(s) });

  const global = (): GlobalOptions => program.opts<GlobalOptions>();

  program
    .command('add')
    .description('register a project and analyze it (or re-analyze with --reanalyze)')
    .argument('<name>', 'project name: letters, digits, ".", "_" or "-"')
    .argument('<path>', 'project root directory')
    .option('--reanalyze', 'analyze an already registered project again (full re-index)')
    .option('--exclude <pattern>', 'extra exclusion in .gitignore syntax (repeatable)', collect)
    .option('--include-default <name>', 'scan a directory excluded by default, e.g. vendor (repeatable)', collect)
    .option('--no-default-excludes', 'do not apply the built-in exclusion list')
    .option('--no-gitignore', 'ignore .gitignore files')
    .option('--max-file-size <size>', 'skip source files larger than this (bytes, or k/m suffix; default 1m)')
    .option('--max-files <n>', 'stop scanning after this many files (default 200000)')
    .option('--json', 'print the analysis summary as JSON')
    .action(async (name: string, path: string, opts) => run(await addCommand(name, path, opts, makeContext(global()))));

  program
    .command('refresh')
    .description('update an indexed project: re-parse changed, added and removed files and re-resolve affected flows')
    .argument('<project>', 'registered project name')
    .option('--json', 'print the refresh summary as JSON')
    .action(async (project: string, opts) => run(await refreshCommand(project, opts, makeContext(global()))));

  program
    .command('log')
    .description('show recent analysis runs, their diagnostics, and application errors')
    .argument('[project]', 'only runs of this project')
    .option('--run <id>', 'show one run in detail, including its diagnostics')
    .option('--limit <n>', 'number of runs to list (default 20)')
    .option('--diagnostics <n>', 'number of diagnostics to show with --run (default 50)')
    .option('--json', 'print machine-readable JSON')
    .action(async (project: string | undefined, opts) => run(await logCommand(project, opts, makeContext(global()))));

  program
    .command('show')
    .description('open the interactive explorer for an indexed project')
    .argument('<project>', 'registered project name')
    .addOption(new Option('--no-tui', 'print a plain-text overview instead of the interactive UI'))
    .option('--check', 'verify index integrity (dangling references, schema) instead of opening the UI')
    .action(async (project: string, opts) => run(await showCommand(project, opts, makeContext(global()))));

  return program;
}

async function main(argv: string[]): Promise<number> {
  let exitCode: number = ExitCode.Success;
  const program = buildProgram((code) => {
    exitCode = code;
  });
  process.once('SIGINT', () => {
    process.stderr.write('\nInterrupted.\n');
    process.exit(ExitCode.Interrupted);
  });
  try {
    await program.parseAsync(argv);
    return exitCode;
  } catch (err) {
    if (err instanceof CommanderError) {
      if (err.code === 'commander.helpDisplayed' || err.code === 'commander.version' || err.code === 'commander.help') {
        return ExitCode.Success;
      }
      return ExitCode.UserError;
    }
    if (err instanceof CliError) {
      process.stderr.write(`${chalk.red('proj:')} ${err.message}\n`);
      return err.exitCode;
    }
    // Unexpected: never swallow, always record.
    const e = err as Error;
    let dataDir: string | undefined;
    try {
      dataDir = resolveDataDir(program.opts<GlobalOptions>().dataDir);
      appendAppLog(dataDir, 'ERROR', 'cli', e?.message ?? String(err), { stack: e?.stack });
    } catch {
      // the error is still printed below
    }
    process.stderr.write(`${chalk.red('proj: internal error:')} ${e?.stack ?? String(err)}\n`);
    return ExitCode.InternalError;
  }
}

// A reader that goes away early (`proj log | head`) is not a failure: Node
// reports EPIPE instead of ending the process with SIGPIPE. Drop the rest of
// the output and keep the command's own exit code.
for (const stream of [process.stdout, process.stderr]) {
  stream.on('error', (err: NodeJS.ErrnoException) => {
    if (err.code !== 'EPIPE' && err.code !== 'ERR_STREAM_DESTROYED') throw err;
  });
}

main(process.argv).then((code) => {
  process.exitCode = code;
});

// Process exit codes. Documented in docs/CLI_REFERENCE.md.
export const ExitCode = {
  /** Command succeeded; for `add`, analysis completed without problems. */
  Success: 0,
  /** Internal failure: engine crash, malformed engine output, SQLite or I/O error, failed analysis. */
  InternalError: 1,
  /** User error: invalid arguments, unknown project, invalid path, duplicate registration. */
  UserError: 2,
  /** Analysis finished but is partial (syntax errors, skipped or unreadable files, limits reached). */
  Partial: 3,
  /** Analysis finished but found no files in a supported language. */
  NoSupportedFiles: 4,
  /** Interrupted by a signal (SIGINT/SIGTERM). */
  Interrupted: 130,
} as const;

export type ExitCodeValue = (typeof ExitCode)[keyof typeof ExitCode];

// Types for the versioned JSON protocol spoken by proj-engine.
// See docs/ARCHITECTURE.md ("Engine protocol").

export const PROTOCOL_VERSION = 1;

export type EngineStatus =
  | 'ok'
  | 'partial'
  | 'invalid_arguments'
  | 'unsupported_operation'
  | 'not_found'
  | 'conflict'
  | 'index_error'
  | 'analysis_failed'
  | 'internal_error';

export const ENGINE_STATUSES: readonly EngineStatus[] = [
  'ok',
  'partial',
  'invalid_arguments',
  'unsupported_operation',
  'not_found',
  'conflict',
  'index_error',
  'analysis_failed',
  'internal_error',
];

export interface EngineRequest {
  protocol: number;
  id: number;
  op: string;
  args: Record<string, unknown>;
  options: { data_dir: string; log_level?: 'debug' | 'info' | 'warn' | 'error' };
}

export interface EngineDiagnostic {
  level: string;
  code: string;
  message: string;
}

export interface EngineError {
  code: string;
  message: string;
  details?: unknown;
}

export interface EngineResponse<T = any> {
  protocol: number;
  id: unknown;
  op: string;
  status: EngineStatus;
  result: T | null;
  diagnostics: EngineDiagnostic[];
  error: EngineError | null;
}

/** Validates the shape of a decoded response; returns an error message or null. */
export function validateEnvelope(value: unknown): string | null {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return 'response is not a JSON object';
  const v = value as Record<string, unknown>;
  if (v.protocol !== PROTOCOL_VERSION) return `unexpected protocol version ${JSON.stringify(v.protocol)}`;
  if (typeof v.status !== 'string' || !ENGINE_STATUSES.includes(v.status as EngineStatus)) {
    return `unknown status ${JSON.stringify(v.status)}`;
  }
  if (!('result' in v) || !Array.isArray(v.diagnostics) || !('error' in v)) return 'response envelope is incomplete';
  return null;
}

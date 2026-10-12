import { useEffect, useRef, useState } from 'react';

import type { EngineSession } from '../engine/client.js';

export interface QueryState<T> {
  data: T | null;
  error: string | null;
  loading: boolean;
}

/** Calls the engine; resolves with the result or rejects with a readable message. */
export async function query<T = any>(session: EngineSession, op: string, args: Record<string, unknown>): Promise<T> {
  const res = await session.request<T>(op, args);
  if ((res.status === 'ok' || res.status === 'partial') && res.result !== null) return res.result;
  throw new Error(res.error?.message ?? `engine reported ${res.status}`);
}

/** Runs a query whenever `key` changes; stale responses are ignored. */
export function useQuery<T = any>(
  session: EngineSession,
  op: string,
  args: Record<string, unknown> | null,
  key: string,
  delayMs = 0,
): QueryState<T> {
  const [state, setState] = useState<QueryState<T>>({ data: null, error: null, loading: args !== null });
  const generation = useRef(0);
  useEffect(() => {
    const gen = ++generation.current;
    if (args === null) {
      setState({ data: null, error: null, loading: false });
      return;
    }
    setState((s) => ({ ...s, loading: true }));
    const timer = setTimeout(() => {
      query<T>(session, op, args).then(
        (data) => {
          if (gen === generation.current) setState({ data, error: null, loading: false });
        },
        (err: Error) => {
          if (gen === generation.current) setState({ data: null, error: err.message, loading: false });
        },
      );
    }, delayMs);
    return () => clearTimeout(timer);
    // `key` captures every input that changes the request.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);
  return state;
}

/**
 * Ref that always holds the latest render's value. Ink re-subscribes input
 * handlers in an effect that runs after the frame is drawn, so a key pressed
 * right after new data appears could otherwise reach a handler that still
 * sees the previous data. Handlers read state through these refs instead.
 */
export function useLatest<T>(value: T): { readonly current: T } {
  const ref = useRef(value);
  ref.current = value;
  return ref;
}

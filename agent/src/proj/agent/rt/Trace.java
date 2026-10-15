package proj.agent.rt;

/**
 * Calls inserted into application methods by {@link Transformer}. They never throw: an internal
 * failure disables recording rather than breaking the application.
 *
 * A trace is one outermost application-code invocation on a thread (a request handled by a servlet,
 * a main() run, a task's run()). Within it, calls are merged into a tree by (parent node, method):
 * the tree keeps the order of first calls and counts repeats, so a loop does not grow it.
 */
public final class Trace {
  static volatile boolean enabled;
  /** Bumped when recording stops; thread states from an older epoch are discarded. */
  static volatile int epoch;

  private static final ThreadLocal<ThreadState> STATE = new ThreadLocal<ThreadState>() {
    @Override
    protected ThreadState initialValue() {
      return new ThreadState();
    }
  };

  private Trace() {}

  private static ThreadState state() {
    ThreadState s = STATE.get();
    if (s.epoch != epoch) s.reset(epoch);
    return s;
  }

  public static void enter(int method) {
    if (!enabled) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.enter(method);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  public static void exit(int method) {
    if (!enabled) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.exit(method, null);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  /** The method is being left by an exception (the added catch-all handler; it rethrows). */
  public static void exitThrow(Throwable ex, int method) {
    if (!enabled) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.exit(method, ex);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  public static void thrown(Throwable ex, int site) {
    if (!enabled || ex == null) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.thrown(ex, site);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  public static void caught(Throwable ex, int site) {
    if (!enabled || ex == null) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.caught(ex, site);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  /** Request object of a handler method (HttpExchange, ServletRequest): names the trace's request. */
  public static void request(Object request) {
    if (!enabled || request == null) return;
    try {
      ThreadState s = state();
      if (!s.busy) s.request(request);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
  }

  static ThreadState current() {
    return STATE.get();
  }
}

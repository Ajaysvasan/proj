package proj.agent.rt;

/**
 * An observed exception, extracted when its trace ends (on the application thread, so no
 * application object is kept afterwards). States, from the evidence the instrumentation has:
 *
 * <ul>
 *   <li>CAUGHT: last handled by an application catch block (not a finally) and not seen again;</li>
 *   <li>WRAPPED: became the cause of another exception observed in the same trace;</li>
 *   <li>ESCAPED: left the trace's outermost application frame (what the framework did next is not seen);</li>
 *   <li>UNCAUGHT: terminated its thread (set by the default uncaught-exception handler);</li>
 *   <li>UNKNOWN: anything else, e.g. it passed into library code between application frames.</li>
 * </ul>
 */
final class ErrorData {
  private static final int MAX_FRAMES = 64;
  private static final int MAX_CAUSE_FRAMES = 16;
  private static final int MAX_CAUSES = 4;

  String type;
  String message;
  String state;
  boolean rethrown;
  String wrappedBy;
  int throwSite = -1;
  int caughtSite = -1;
  int escapedFrom = -1;
  Frame[] frames;
  int framesOmitted;
  Cause[] causes;
  int suppressed;
  int[] eventKinds;
  int[] eventRefs;
  int droppedEvents;

  static final class Frame {
    String cls;
    String method;
    String file;
    int line;
    boolean app;
  }

  static final class Cause {
    String type;
    String message;
    Frame[] frames;
    int framesOmitted;
  }

  static ErrorData[] extract(TraceTree tree, Options options) {
    ErrorData[] out = new ErrorData[tree.errorCount];
    for (int i = 0; i < tree.errorCount; i++) {
      ErrorRec r = tree.errors[i];
      ErrorData d = new ErrorData();
      d.type = r.ex.getClass().getName();
      d.message = message(r.ex, options);
      boolean caught = false;
      for (int k = 0; k < r.count; k++) {
        int ref = r.refs[k];
        switch (r.kinds[k]) {
          case ErrorRec.THROWN:
            if (caught) d.rethrown = true;
            caught = false;
            if (d.throwSite < 0) d.throwSite = ref;
            break;
          case ErrorRec.EXIT:
            caught = false;
            break;
          default: {
            Methods.Site s = Methods.site(ref);
            if (s != null && s.kind == Methods.CATCH) {
              caught = true;
              d.caughtSite = ref;
            }
          }
        }
      }
      for (int j = 0; j < tree.errorCount; j++) {
        if (j != i && causedBy(tree.errors[j].ex, r.ex)) {
          d.wrappedBy = tree.errors[j].ex.getClass().getName();
          break;
        }
      }
      if (r.escaped) {
        d.state = "ESCAPED";
        d.escapedFrom = tree.method[0];
      } else if (d.wrappedBy != null) {
        d.state = "WRAPPED";
      } else {
        d.state = caught ? "CAUGHT" : "UNKNOWN";
      }
      describe(d, r.ex, options);
      d.eventKinds = java.util.Arrays.copyOf(r.kinds, r.count);
      d.eventRefs = java.util.Arrays.copyOf(r.refs, r.count);
      d.droppedEvents = r.droppedEvents;
      out[i] = d;
    }
    return out;
  }

  /** Stack frames, causes and suppressed count of an exception (no trace context). */
  static void describe(ErrorData d, Throwable ex, Options options) {
    StackTraceElement[] st = ex.getStackTrace();
    d.frames = frames(st, MAX_FRAMES, options);
    d.framesOmitted = Math.max(0, st.length - MAX_FRAMES);
    java.util.ArrayList<Cause> causes = new java.util.ArrayList<Cause>();
    Throwable c = ex.getCause();
    while (c != null && c != ex && causes.size() < MAX_CAUSES) {
      Cause cause = new Cause();
      cause.type = c.getClass().getName();
      cause.message = message(c, options);
      StackTraceElement[] cs = c.getStackTrace();
      cause.frames = frames(cs, MAX_CAUSE_FRAMES, options);
      cause.framesOmitted = Math.max(0, cs.length - MAX_CAUSE_FRAMES);
      causes.add(cause);
      c = c.getCause();
    }
    d.causes = causes.toArray(new Cause[0]);
    d.suppressed = ex.getSuppressed().length;
  }

  private static Frame[] frames(StackTraceElement[] st, int max, Options options) {
    Frame[] out = new Frame[Math.min(st.length, max)];
    for (int i = 0; i < out.length; i++) {
      Frame f = new Frame();
      f.cls = st[i].getClassName();
      f.method = st[i].getMethodName();
      f.file = st[i].getFileName();
      f.line = st[i].getLineNumber();
      f.app = options.includes(f.cls.replace('.', '/'));
      out[i] = f;
    }
    return out;
  }

  private static String message(Throwable ex, Options options) {
    if (!options.messages) return null;
    String m;
    try {
      m = ex.getMessage();
    } catch (Throwable t) {
      return null;
    }
    if (m == null) return null;
    return m.length() > options.maxMessage ? m.substring(0, options.maxMessage) + "..." : m;
  }

  private static boolean causedBy(Throwable outer, Throwable inner) {
    Throwable c = outer.getCause();
    for (int depth = 0; c != null && depth < 8; depth++) {
      if (c == inner) return true;
      if (c == c.getCause()) break;
      c = c.getCause();
    }
    return false;
  }
}

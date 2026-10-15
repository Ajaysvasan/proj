package proj.agent.rt;

import java.lang.instrument.Instrumentation;
import java.lang.management.ManagementFactory;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/** Agent lifecycle: start (at JVM startup or on attach), recording on/off, detach, JVM exit. */
public final class AgentState {
  static final String VERSION = "0.1.0";

  private static boolean active;
  static Options options = new Options();
  private static Instrumentation inst;
  private static Transformer transformer;
  static Sender sender;
  static String session;
  static String mode;  // startup|attach
  static long loadedMillis;
  /** Recording since the JVM started, never interrupted (startup coverage is complete). */
  static volatile boolean sinceStart;
  private static Thread hook;

  private static final AtomicLong traceIds = new AtomicLong();
  static final ConcurrentHashMap<Long, TraceTree> open = new ConcurrentHashMap<Long, TraceTree>();
  static final AtomicLong instrumentedClasses = new AtomicLong();
  static final AtomicLong instrumentedMethods = new AtomicLong();
  static final AtomicLong transformFailures = new AtomicLong();
  static final AtomicLong internalErrors = new AtomicLong();
  static final AtomicLong abandonedTraces = new AtomicLong();
  static final AtomicLong droppedItems = new AtomicLong();
  static final AtomicLong droppedBytes = new AtomicLong();
  static volatile String lastTransformError;

  private AgentState() {}

  public static synchronized void start(String args, Instrumentation instrumentation, boolean attached) {
    Options o = Options.parse(args);
    if (o.project.isEmpty() || o.service.isEmpty() || o.include.isEmpty()) {
      System.err.println("proj-agent: the project=, service= and include= options are required; agent not started");
      return;
    }
    if (active) {
      // Loaded again (a second attach): keep this session; ask for monitoring again.
      if (sender != null) sender.reconnectNow();
      return;
    }
    options = o;
    inst = instrumentation;
    session = UUID.randomUUID().toString();
    mode = attached ? "attach" : "startup";
    loadedMillis = System.currentTimeMillis();
    sinceStart = !attached;
    Trace.epoch++;
    Trace.enabled = true;  // startup: recorded into the bounded queue until the service answers
    transformer = new Transformer(o);
    inst.addTransformer(transformer, true);
    if (attached) retransform(loadedApplicationClasses());
    Uncaught.install();
    sender = new Sender(o);
    sender.start();
    hook = new Thread(new Runnable() {
      @Override
      public void run() {
        shutdown();
      }
    }, "proj-agent-shutdown");
    Runtime.getRuntime().addShutdownHook(hook);
    active = true;
  }

  static long pid() {
    String name = ManagementFactory.getRuntimeMXBean().getName();  // "pid@host" (Java 8 compatible)
    try {
      return Long.parseLong(name.substring(0, name.indexOf('@')));
    } catch (RuntimeException e) {
      return -1;
    }
  }

  static long jvmStartMillis() {
    return ManagementFactory.getRuntimeMXBean().getStartTime();
  }

  // ---- traces ---------------------------------------------------------------

  static TraceTree openTree(int method, Thread thread) {
    TraceTree t = new TraceTree(traceIds.incrementAndGet(), method, thread, options.maxNodes, options.maxErrors);
    open.put(t.id, t);
    return t;
  }

  /** A thread's open trace discarded (recording stopped while it ran). */
  static void closeTree(TraceTree t) {
    if (open.remove(t.id) != null) abandonedTraces.incrementAndGet();
  }

  static void finishTree(TraceTree t) {
    open.remove(t.id);
    t.durationNanos = System.nanoTime() - t.startNanos;
    t.status = t.truncated ? "truncated" : "complete";
    t.errorData = ErrorData.extract(t, options);
    t.errors = null;  // no application objects are kept past the trace
    if (sender != null) sender.offerTrace(t, t.status);
  }

  static void uncaught(Thread thread, Throwable ex) {
    long trace = -1;
    if (thread == Thread.currentThread()) {
      ThreadState s = Trace.current();
      if (s.lastEscaped == ex) trace = s.lastTrace;
      s.lastEscaped = null;
    }
    ErrorData d = new ErrorData();
    d.type = ex.getClass().getName();
    d.state = "UNCAUGHT";
    String m = null;
    if (options.messages) {
      try {
        m = ex.getMessage();
      } catch (Throwable t) {
        m = null;
      }
    }
    d.message = m == null ? null : (m.length() > options.maxMessage ? m.substring(0, options.maxMessage) + "..." : m);
    ErrorData.describe(d, ex, options);
    if (sender != null) sender.offerUncaught(d, trace, thread);
  }

  // ---- counters -------------------------------------------------------------

  static void instrumented(int methods) {
    instrumentedClasses.incrementAndGet();
    instrumentedMethods.addAndGet(methods);
  }

  static void transformFailed(String cls, Throwable t) {
    transformFailures.incrementAndGet();
    lastTransformError = cls + ": " + t;
  }

  static void internalError(Throwable t) {
    if (internalErrors.incrementAndGet() == 100) {
      Trace.enabled = false;  // something is systematically wrong: stop rather than slow the application
      System.err.println("proj-agent: recording stopped after repeated internal errors; last: " + t);
    }
  }

  // ---- recording, detach, exit ----------------------------------------------

  static synchronized void setRecording(boolean on) {
    if (on == Trace.enabled) return;
    if (!on) sinceStart = false;
    Trace.epoch++;  // in-flight thread states are discarded on their next call
    Trace.enabled = on;
  }

  /** Monitoring stopped: restore the original classes, flush, disconnect. The application keeps running. */
  static synchronized void detach(String reason) {
    if (!active) return;
    setRecording(false);
    try {
      inst.removeTransformer(transformer);
      List<Class<?>> restore = new ArrayList<Class<?>>();
      for (Class<?> c : inst.getAllLoadedClasses()) {
        if (transformer.transformed.contains(c.getName().replace('.', '/')) && inst.isModifiableClass(c)) restore.add(c);
      }
      retransform(restore);
    } catch (Throwable t) {
      System.err.println("proj-agent: restoring classes failed: " + t);
    }
    Uncaught.uninstall();
    for (TraceTree t : open.values()) closeTree(t);
    if (sender != null) sender.close(reason, 2000);
    try {
      if (hook != null && Thread.currentThread() != hook) Runtime.getRuntime().removeShutdownHook(hook);
    } catch (IllegalStateException e) {
      // already shutting down
    }
    active = false;
  }

  private static void shutdown() {
    // Open traces (a server's main thread, requests in flight) are sent as incomplete.
    if (sender == null) return;
    for (TraceTree t : open.values()) sender.offerTrace(t, "incomplete");
    sender.close("jvm_exit", 2000);
  }

  private static List<Class<?>> loadedApplicationClasses() {
    List<Class<?>> out = new ArrayList<Class<?>>();
    for (Class<?> c : inst.getAllLoadedClasses()) {
      if (c.isArray() || c.isPrimitive() || c.isInterface() || c.getClassLoader() == null) continue;
      if (options.includes(c.getName().replace('.', '/')) && inst.isModifiableClass(c)) out.add(c);
    }
    return out;
  }

  private static void retransform(List<Class<?>> classes) {
    for (int i = 0; i < classes.size(); i += 64) {
      List<Class<?>> batch = classes.subList(i, Math.min(classes.size(), i + 64));
      try {
        inst.retransformClasses(batch.toArray(new Class<?>[0]));
      } catch (Throwable t) {
        // One bad class fails the batch: retry one by one.
        for (Class<?> c : batch) {
          try {
            inst.retransformClasses(c);
          } catch (Throwable e) {
            transformFailed(c.getName(), e);
          }
        }
      }
    }
  }
}

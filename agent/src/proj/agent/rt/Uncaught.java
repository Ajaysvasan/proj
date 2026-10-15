package proj.agent.rt;

import java.io.PrintStream;

/**
 * Default uncaught-exception handler: reports exceptions that terminate a thread, then behaves as
 * before (the previous default handler, or the JDK's standard message). Threads with their own
 * handler, or whose group overrides uncaughtException, are not seen.
 */
final class Uncaught implements Thread.UncaughtExceptionHandler {
  private final Thread.UncaughtExceptionHandler previous;
  private static Uncaught installed;

  private Uncaught(Thread.UncaughtExceptionHandler previous) {
    this.previous = previous;
  }

  static synchronized void install() {
    if (installed != null) return;
    installed = new Uncaught(Thread.getDefaultUncaughtExceptionHandler());
    Thread.setDefaultUncaughtExceptionHandler(installed);
  }

  static synchronized void uninstall() {
    if (installed == null) return;
    if (Thread.getDefaultUncaughtExceptionHandler() == installed) Thread.setDefaultUncaughtExceptionHandler(installed.previous);
    installed = null;
  }

  @Override
  public void uncaughtException(Thread thread, Throwable ex) {
    try {
      if (Trace.enabled) AgentState.uncaught(thread, ex);
    } catch (Throwable t) {
      AgentState.internalError(t);
    }
    if (previous != null) {
      previous.uncaughtException(thread, ex);
    } else if (!"java.lang.ThreadDeath".equals(ex.getClass().getName())) {  // by name: removed in newer JDKs
      PrintStream err = System.err;
      err.print("Exception in thread \"" + thread.getName() + "\" ");
      ex.printStackTrace(err);
    }
  }
}

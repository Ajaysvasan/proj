package proj.agent;

import java.io.File;
import java.lang.instrument.Instrumentation;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.jar.JarFile;

/**
 * Entry points of the proj runtime agent.
 *
 * <ul>
 *   <li>{@code -javaagent:proj-agent.jar=project=NAME,service=FILE,include=PKG:PKG} loads it at JVM
 *       startup (complete startup coverage);</li>
 *   <li>{@code java -jar proj-agent.jar --attach PID ARGS} attaches it to a running JVM of the same
 *       user (what `proj start dynamic NAME --pid PID` runs); earlier startup is not recoverable.</li>
 * </ul>
 *
 * This class is loaded by the system class loader. Everything else lives in proj.agent.rt and is
 * loaded from the bootstrap class path, so instrumented classes of any class loader can call it.
 */
public final class Agent {
  private Agent() {}

  public static void premain(String args, Instrumentation inst) {
    start(args, inst, false);
  }

  public static void agentmain(String args, Instrumentation inst) {
    start(args, inst, true);
  }

  private static void start(String args, Instrumentation inst, boolean attached) {
    try {
      String jar = jarPath();
      // Once per JVM: a second attach finds the jar already on the bootstrap path.
      if (System.getProperty("proj.agent.boot") == null) {
        inst.appendToBootstrapClassLoaderSearch(new JarFile(jar));
        System.setProperty("proj.agent.boot", jar);
      }
      // Resolved through the bootstrap loader (parent first), never through this class's loader.
      Class<?> state = Class.forName("proj.agent.rt.AgentState", true, null);
      Method m = state.getMethod("start", String.class, Instrumentation.class, boolean.class);
      m.invoke(null, args == null ? "" : args, inst, attached);
    } catch (Throwable t) {
      System.err.println("proj-agent: not started: " + t);
    }
  }

  private static String jarPath() throws Exception {
    URL url = Agent.class.getProtectionDomain().getCodeSource().getLocation();
    return new File(url.toURI()).getAbsolutePath();
  }

  /** {@code --attach PID AGENT_ARGS}: loads this jar into the JVM with that process id. */
  public static void main(String[] argv) {
    if (argv.length != 3 || !"--attach".equals(argv[0])) {
      System.err.println("usage: java -jar proj-agent.jar --attach <pid> <agent-options>");
      System.exit(2);
    }
    try {
      Class<?> vm = attachApi();
      Object target = vm.getMethod("attach", String.class).invoke(null, argv[1]);
      try {
        vm.getMethod("loadAgent", String.class, String.class).invoke(target, jarPath(), argv[2]);
      } finally {
        vm.getMethod("detach").invoke(target);
      }
      System.out.println("{\"attached\":true}");
    } catch (Throwable t) {
      Throwable cause = t instanceof java.lang.reflect.InvocationTargetException && t.getCause() != null ? t.getCause() : t;
      String msg = String.valueOf(cause).replace('\\', '/').replace('"', '\'');
      System.out.println("{\"attached\":false,\"error\":\"" + msg + "\"}");
      System.exit(1);
    }
  }

  // JDK 9+: module jdk.attach. JDK 8: lib/tools.jar of the JDK running this command.
  private static Class<?> attachApi() throws Exception {
    try {
      return Class.forName("com.sun.tools.attach.VirtualMachine");
    } catch (ClassNotFoundException e) {
      File tools = new File(System.getProperty("java.home"), "../lib/tools.jar");
      if (!tools.isFile()) {
        throw new IllegalStateException("the attach API is missing: run this with a JDK (not a JRE)");
      }
      URLClassLoader loader = new URLClassLoader(new URL[] {tools.toURI().toURL()});
      return Class.forName("com.sun.tools.attach.VirtualMachine", true, loader);
    }
  }
}

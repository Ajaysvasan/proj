package proj.agent.rt;

import java.util.ArrayList;
import java.util.List;

/**
 * Agent options: comma-separated key=value pairs, for example
 * {@code project=shop,service=/home/me/.local/share/proj/dynamic/service.json,include=com.acme:org.acme}.
 */
public final class Options {
  /** Registered proj project the JVM belongs to. */
  public String project = "";
  /** Service file written by `proj start dynamic`: the local port and token to report to. */
  public String service = "";
  /** Application packages as internal-name prefixes ("com/acme/"); "" is the default package. */
  public final List<String> include = new ArrayList<String>();
  /** Record exception messages (truncated to maxMessage characters). */
  public boolean messages = true;
  public int maxMessage = 300;
  /** Call-tree nodes kept per trace; further distinct calls are counted as dropped. */
  public int maxNodes = 5000;
  /** Exceptions kept per trace. */
  public int maxErrors = 64;
  /** Bounded send queue: items and approximate bytes. */
  public int queueItems = 4096;
  public long queueBytes = 16L << 20;
  /** Open traces older than this are sent as snapshots (a server's main thread never returns). */
  public int snapshotMillis = 5000;

  static Options parse(String args) {
    Options o = new Options();
    for (String part : args.split(",")) {
      int eq = part.indexOf('=');
      if (eq <= 0) continue;
      String key = part.substring(0, eq).trim();
      String value = part.substring(eq + 1).trim();
      if (key.equals("project")) o.project = value;
      else if (key.equals("service")) o.service = value;
      else if (key.equals("include")) {
        for (String pkg : value.split("[:;]")) {
          String p = pkg.trim();
          if (p.equals("<default>")) o.include.add("");
          else if (!p.isEmpty()) o.include.add(p.replace('.', '/') + "/");
        }
      } else if (key.equals("messages")) o.messages = !value.equals("off");
      else if (key.equals("maxMessage")) o.maxMessage = intValue(value, o.maxMessage, 0, 10000);
      else if (key.equals("maxNodes")) o.maxNodes = intValue(value, o.maxNodes, 16, 1000000);
      else if (key.equals("maxErrors")) o.maxErrors = intValue(value, o.maxErrors, 1, 10000);
      else if (key.equals("queueItems")) o.queueItems = intValue(value, o.queueItems, 16, 1000000);
      else if (key.equals("queueMiB")) o.queueBytes = (long) intValue(value, 16, 1, 4096) << 20;
      else if (key.equals("snapshotMillis")) o.snapshotMillis = intValue(value, o.snapshotMillis, 100, 3600000);
    }
    return o;
  }

  private static int intValue(String s, int def, int min, int max) {
    try {
      int v = Integer.parseInt(s);
      return v < min ? min : (v > max ? max : v);
    } catch (NumberFormatException e) {
      return def;
    }
  }

  /** Application code: inside an included package, never the JDK or this agent. */
  boolean includes(String internalName) {
    if (internalName.startsWith("java/") || internalName.startsWith("javax/") || internalName.startsWith("jdk/")
        || internalName.startsWith("sun/") || internalName.startsWith("com/sun/") || internalName.startsWith("proj/agent/")) {
      return false;
    }
    for (String p : include) {
      if (p.isEmpty() ? internalName.indexOf('/') < 0 : internalName.startsWith(p)) return true;
    }
    return false;
  }

  String includeList() {
    StringBuilder sb = new StringBuilder();
    for (String p : include) {
      if (sb.length() > 0) sb.append(':');
      sb.append(p.isEmpty() ? "<default>" : p.substring(0, p.length() - 1).replace('/', '.'));
    }
    return sb.toString();
  }
}

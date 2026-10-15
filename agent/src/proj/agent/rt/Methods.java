package proj.agent.rt;

import java.util.HashMap;

/**
 * Ids of instrumented methods and of throw/catch sites, assigned when a class is transformed.
 * Ids stay stable for the life of the JVM (a retransformed class gets the same ids), so frames
 * still running old code after a detach and re-attach report valid ids.
 */
final class Methods {
  static final int THROW = 0;
  static final int CATCH = 1;
  static final int FINALLY = 2;

  static final class Info {
    final String owner;  // internal name: com/acme/Foo$Bar
    final String name;
    final String desc;
    final String source;
    int firstLine = Integer.MAX_VALUE;
    int lastLine = 0;

    Info(String owner, String name, String desc, String source) {
      this.owner = owner;
      this.name = name;
      this.desc = desc;
      this.source = source;
    }
  }

  static final class Site {
    final int method;
    final int line;
    final int kind;  // THROW, CATCH or FINALLY
    final String type;  // caught type (internal name), "" for a finally/any handler

    Site(int method, int line, int kind, String type) {
      this.method = method;
      this.line = line;
      this.kind = kind;
      this.type = type;
    }
  }

  private static final Object LOCK = new Object();
  private static Info[] methods = new Info[1024];
  private static int methodCount;
  private static final HashMap<String, Integer> methodIds = new HashMap<String, Integer>();
  private static Site[] sites = new Site[1024];
  private static int siteCount;
  private static final HashMap<String, Integer> siteIds = new HashMap<String, Integer>();

  private Methods() {}

  static int method(String loaderKey, String owner, String name, String desc, String source) {
    String key = loaderKey + ' ' + owner + '.' + name + desc;
    synchronized (LOCK) {
      Integer id = methodIds.get(key);
      if (id != null) return id;
      if (methodCount == methods.length) methods = java.util.Arrays.copyOf(methods, methodCount * 2);
      methods[methodCount] = new Info(owner, name, desc, source);
      methodIds.put(key, methodCount);
      return methodCount++;
    }
  }

  static void line(int method, int line) {
    synchronized (LOCK) {
      Info i = methods[method];
      if (line < i.firstLine) i.firstLine = line;
      if (line > i.lastLine) i.lastLine = line;
    }
  }

  static Info info(int id) {
    synchronized (LOCK) {
      return id >= 0 && id < methodCount ? methods[id] : null;
    }
  }

  /** ordinal tells apart several sites of one kind on the same line. */
  static int site(int method, int line, int kind, String type, int ordinal) {
    String key = method + " " + line + " " + kind + " " + type + " " + ordinal;
    synchronized (LOCK) {
      Integer id = siteIds.get(key);
      if (id != null) return id;
      if (siteCount == sites.length) sites = java.util.Arrays.copyOf(sites, siteCount * 2);
      sites[siteCount] = new Site(method, line, kind, type);
      siteIds.put(key, siteCount);
      return siteCount++;
    }
  }

  static Site site(int id) {
    synchronized (LOCK) {
      return id >= 0 && id < siteCount ? sites[id] : null;
    }
  }

  static int count() {
    synchronized (LOCK) {
      return methodCount;
    }
  }
}

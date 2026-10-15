package proj.agent.rt;

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;

/** Per-thread call stack of application frames and the trace being built. */
final class ThreadState {
  private static final int MAX_DEPTH = 4096;

  int epoch = -1;
  boolean busy;  // inside the agent's own work (e.g. an application getMessage()): ignore nested calls
  private int[] methods = new int[64];
  private int[] nodes = new int[64];
  private int depth;
  private int overflow;  // frames beyond MAX_DEPTH, not recorded
  private TraceTree tree;
  /** Last finished trace on this thread and the exception that escaped it, for the uncaught handler. */
  long lastTrace = -1;
  Throwable lastEscaped;

  void reset(int epoch) {
    this.epoch = epoch;
    depth = 0;
    overflow = 0;
    if (tree != null) AgentState.closeTree(tree);
    tree = null;
    lastEscaped = null;
  }

  void enter(int method) {
    if (depth == 0 && overflow == 0) {
      tree = AgentState.openTree(method, Thread.currentThread());
      push(method, 0);
      return;
    }
    if (depth >= MAX_DEPTH) {
      overflow++;
      tree.truncated = true;
      return;
    }
    int parent = nodes[depth - 1];
    push(method, parent < 0 ? -1 : tree.child(parent, method));
  }

  private void push(int method, int node) {
    if (depth == methods.length) {
      methods = Arrays.copyOf(methods, depth * 2);
      nodes = Arrays.copyOf(nodes, depth * 2);
    }
    methods[depth] = method;
    nodes[depth] = node;
    depth++;
  }

  /** Normal return (ex == null) or exit by exception. */
  void exit(int method, Throwable ex) {
    if (overflow > 0) {
      overflow--;
      return;
    }
    if (depth == 0) return;  // entered before recording started
    int at = find(method);
    if (at < 0) return;  // not on the stack (re-instrumented while running): ignore
    // Frames above it left without an exit event: an exception passed through them (constructors
    // and methods without the added handler).
    for (int i = depth - 1; i > at; i--) {
      if (nodes[i] >= 0) tree.exc[nodes[i]]++;
    }
    if (ex != null) {
      if (nodes[at] >= 0) tree.exc[nodes[at]]++;
      ErrorRec e = tree.error(ex);
      if (e != null) e.event(ErrorRec.EXIT, method);
    }
    depth = at;
    if (depth == 0) finish(ex);
  }

  void thrown(Throwable ex, int site) {
    if (depth == 0 || tree == null) return;
    ErrorRec e = tree.error(ex);
    if (e != null) e.event(ErrorRec.THROWN, site);
  }

  void caught(Throwable ex, int site) {
    if (depth == 0 || tree == null) return;
    Methods.Site s = Methods.site(site);
    if (s != null) {
      int at = find(s.method);
      if (at >= 0) {
        // The exception passed through the frames above the catching method.
        ErrorRec e = tree.error(ex);
        for (int i = depth - 1; i > at; i--) {
          if (nodes[i] >= 0) tree.exc[nodes[i]]++;
          if (e != null) e.event(ErrorRec.EXIT, methods[i]);
        }
        depth = at + 1;
      }
    }
    ErrorRec e = tree.error(ex);
    if (e != null) e.event(ErrorRec.CAUGHT, site);
  }

  private int find(int method) {
    for (int i = depth - 1; i >= 0; i--) {
      if (methods[i] == method) return i;
    }
    return -1;
  }

  private void finish(Throwable escaped) {
    busy = true;
    try {
      TraceTree t = tree;
      tree = null;
      if (escaped != null) {
        ErrorRec e = t.error(escaped);
        if (e != null) e.escaped = true;
      }
      lastTrace = t.id;
      lastEscaped = escaped;
      AgentState.finishTree(t);
    } finally {
      busy = false;
    }
  }

  // ---- request names ------------------------------------------------------

  private static final String[][] REQUEST_APIS = {
    {"com.sun.net.httpserver.HttpExchange", "getRequestMethod", "getRequestURI"},
    {"javax.servlet.http.HttpServletRequest", "getMethod", "getRequestURI"},
    {"jakarta.servlet.http.HttpServletRequest", "getMethod", "getRequestURI"},
  };
  private static final Method[] NONE = new Method[0];
  private static final ConcurrentHashMap<Class<?>, Method[]> REQUEST_METHODS = new ConcurrentHashMap<Class<?>, Method[]>();

  void request(Object request) {
    if (depth != 1 || tree == null || tree.httpMethod != null) return;  // only the trace's outermost frame
    busy = true;
    try {
      Method[] m = REQUEST_METHODS.get(request.getClass());
      if (m == null) {
        m = lookup(request.getClass());
        if (REQUEST_METHODS.size() < 256) REQUEST_METHODS.put(request.getClass(), m);
      }
      if (m.length == 0) return;
      Object verb = m[0].invoke(request);
      Object uri = m[1].invoke(request);
      String path = uri instanceof java.net.URI ? ((java.net.URI) uri).getRawPath() : String.valueOf(uri);
      if (verb != null && path != null) {
        tree.httpMethod = String.valueOf(verb);
        tree.httpPath = path.length() > 512 ? path.substring(0, 512) : path;  // the path only, never the query
      }
    } catch (Throwable t) {
      // not a request object we understand
    } finally {
      busy = false;
    }
  }

  // Methods of the public request type, so non-public implementation classes do not block access.
  private static Method[] lookup(Class<?> cls) {
    for (Class<?> c = cls; c != null; c = c.getSuperclass()) {
      Method[] m = match(c);
      if (m != null) return m;
      for (Class<?> i : allInterfaces(c)) {
        m = match(i);
        if (m != null) return m;
      }
    }
    return NONE;
  }

  private static Method[] match(Class<?> c) {
    for (String[] api : REQUEST_APIS) {
      if (api[0].equals(c.getName())) {
        try {
          return new Method[] {c.getMethod(api[1]), c.getMethod(api[2])};
        } catch (NoSuchMethodException e) {
          return null;
        }
      }
    }
    return null;
  }

  private static java.util.List<Class<?>> allInterfaces(Class<?> c) {
    java.util.List<Class<?>> out = new java.util.ArrayList<Class<?>>();
    java.util.ArrayDeque<Class<?>> todo = new java.util.ArrayDeque<Class<?>>(Arrays.asList(c.getInterfaces()));
    while (!todo.isEmpty() && out.size() < 64) {
      Class<?> i = todo.poll();
      out.add(i);
      todo.addAll(Arrays.asList(i.getInterfaces()));
    }
    return out;
  }
}

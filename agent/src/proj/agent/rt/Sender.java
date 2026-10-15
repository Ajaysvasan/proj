package proj.agent.rt;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.Charset;
import java.util.BitSet;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Sends observations to the proj service over a local TCP connection (Java 8 has no Unix sockets;
 * the port and a random token come from the service file, readable only by its owner).
 *
 * Items wait in a bounded queue. When it is full, or while no service is reachable and the queue
 * fills, new items are dropped and counted; the counts are reported, so a trace with gaps is never
 * presented as complete. Reconnects back off from 0.5 s to 30 s.
 */
final class Sender implements Runnable {
  private static final Charset UTF8 = Charset.forName("UTF-8");
  private static final long STATS_MILLIS = 5000;

  private final Options options;
  private final ArrayBlockingQueue<Object> queue;
  private final AtomicLong queuedBytes = new AtomicLong();
  private final BitSet sentMethods = new BitSet();  // definitions sent on this connection
  private Thread thread;
  private Socket socket;
  private Writer out;
  private volatile boolean connected;
  private volatile boolean closing;
  private volatile String closeReason;
  private volatile boolean reconnectNow;
  private long backoff = 500;
  private long nextAttempt;
  private long lastStats;
  private String lastStatsLine = "";
  private String rejected = "";

  private static final class Item {
    final TraceTree tree;
    final String status;
    final ErrorData uncaught;
    final long trace;
    final long threadId;
    final String threadName;
    final long time = System.currentTimeMillis();
    final long bytes;

    Item(TraceTree tree, String status, ErrorData uncaught, long trace, Thread thread, long bytes) {
      this.tree = tree;
      this.status = status;
      this.uncaught = uncaught;
      this.trace = trace;
      this.threadId = thread == null ? -1 : thread.getId();
      this.threadName = thread == null ? null : thread.getName();
      this.bytes = bytes;
    }
  }

  Sender(Options options) {
    this.options = options;
    this.queue = new ArrayBlockingQueue<Object>(options.queueItems);
  }

  void start() {
    thread = new Thread(this, "proj-agent-sender");
    thread.setDaemon(true);
    thread.start();
  }

  void offerTrace(TraceTree t, String status) {
    long bytes = 64L + t.size * 32L + (t.errorData == null ? 0 : t.errorData.length * 4096L);
    offer(new Item(t, status, null, -1, null, bytes), bytes);
  }

  void offerUncaught(ErrorData d, long trace, Thread thread) {
    offer(new Item(null, null, d, trace, thread, 4096), 4096);
  }

  private void offer(Item item, long bytes) {
    if (closing && Thread.currentThread() != thread && !"jvm_exit".equals(closeReason)) return;
    if (queuedBytes.get() + bytes > options.queueBytes || !queue.offer(item)) {
      AgentState.droppedItems.incrementAndGet();
      AgentState.droppedBytes.addAndGet(bytes);
      return;
    }
    queuedBytes.addAndGet(bytes);
  }

  void reconnectNow() {
    reconnectNow = true;
  }

  /** Sends what is queued (up to {@code waitMillis}), says goodbye and disconnects. */
  void close(String reason, long waitMillis) {
    closeReason = reason;
    closing = true;
    if (thread == null) return;
    try {
      thread.join(waitMillis);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
    disconnect();
  }

  @Override
  public void run() {
    while (true) {
      if (closing && (queue.isEmpty() || !connected)) break;
      if (!connected) {
        if (!connect()) {
          sleep(100);
          continue;
        }
      }
      try {
        Object o = queue.poll(250, TimeUnit.MILLISECONDS);
        if (o != null) {
          Item item = (Item) o;
          queuedBytes.addAndGet(-item.bytes);
          write(item);
          if (queue.isEmpty()) out.flush();
        } else {
          out.flush();
        }
        periodic();
      } catch (InterruptedException e) {
        break;
      } catch (IOException e) {
        disconnect();
      } catch (Throwable t) {
        AgentState.internalError(t);
      }
    }
    if (connected) {
      try {
        line(stats());
        line("{\"type\":\"bye\",\"session\":\"" + AgentState.session + "\",\"reason\":"
            + Json.str(new StringBuilder(), closeReason == null ? "stopped" : closeReason) + "}");
        out.flush();
      } catch (IOException e) {
        // the service went away first
      }
    }
  }

  private static void sleep(long ms) {
    try {
      Thread.sleep(ms);
    } catch (InterruptedException e) {
      Thread.currentThread().interrupt();
    }
  }

  // ---- connection -----------------------------------------------------------

  private boolean connect() {
    long now = System.currentTimeMillis();
    if (!reconnectNow && now < nextAttempt) return false;
    reconnectNow = false;
    nextAttempt = now + backoff;
    backoff = Math.min(backoff * 2, 30000);
    Socket s = null;
    try {
      String service = readService(options.service);
      String port = service == null ? null : Json.field(service, "port");
      String token = service == null ? null : Json.field(service, "token");
      if (port == null || token == null) return false;  // no service running yet
      s = new Socket();
      s.connect(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), Integer.parseInt(port)), 1000);
      s.setTcpNoDelay(true);
      Writer w = new BufferedWriter(new OutputStreamWriter(s.getOutputStream(), UTF8), 1 << 16);
      BufferedReader r = new BufferedReader(new InputStreamReader(s.getInputStream(), UTF8));
      w.write(hello(token));
      w.write('\n');
      w.flush();
      s.setSoTimeout(5000);
      String welcome = r.readLine();
      s.setSoTimeout(0);
      if (welcome == null || !"true".equals(Json.field(welcome, "accept"))) {
        String reason = welcome == null ? "no answer" : Json.field(welcome, "reason");
        if (!String.valueOf(reason).equals(rejected)) System.err.println("proj-agent: the proj service refused this JVM: " + reason);
        rejected = String.valueOf(reason);
        nextAttempt = System.currentTimeMillis() + 30000;
        s.close();
        return false;
      }
      boolean record = "true".equals(Json.field(welcome, "enabled"));
      if (!record) queue.clear();  // not monitored: nothing recorded so far is kept
      queuedBytes.set(0);
      AgentState.setRecording(record);
      socket = s;
      out = w;
      sentMethods.clear();
      connected = true;
      backoff = 500;
      rejected = "";
      startControlReader(s, r);
      return true;
    } catch (Throwable t) {
      if (s != null) {
        try {
          s.close();
        } catch (IOException e) {
          // ignore
        }
      }
      return false;
    }
  }

  private static String readService(String path) {
    File f = new File(path);
    if (!f.isFile() || f.length() > 4096) return null;
    byte[] buf = new byte[(int) f.length()];
    FileInputStream in = null;
    try {
      in = new FileInputStream(f);
      int n = 0;
      while (n < buf.length) {
        int r = in.read(buf, n, buf.length - n);
        if (r < 0) break;
        n += r;
      }
      return new String(buf, 0, n, UTF8);
    } catch (IOException e) {
      return null;
    } finally {
      if (in != null) {
        try {
          in.close();
        } catch (IOException e) {
          // ignore
        }
      }
    }
  }

  private String hello(String token) {
    StringBuilder sb = new StringBuilder("{\"type\":\"hello\",\"token\":");
    Json.str(sb, token).append(",\"project\":");
    Json.str(sb, options.project).append(",\"session\":");
    Json.str(sb, AgentState.session).append(",\"pid\":").append(AgentState.pid());
    sb.append(",\"mode\":");
    Json.str(sb, AgentState.mode).append(",\"agent\":");
    Json.str(sb, AgentState.VERSION).append(",\"jvm\":{\"version\":");
    Json.str(sb, System.getProperty("java.version")).append(",\"vendor\":");
    Json.str(sb, System.getProperty("java.vendor")).append(",\"start_ms\":").append(AgentState.jvmStartMillis());
    sb.append("},\"loaded_ms\":").append(AgentState.loadedMillis);
    sb.append(",\"since_start\":").append(AgentState.sinceStart);
    sb.append(",\"include\":");
    Json.str(sb, options.includeList()).append('}');
    return sb.toString();
  }

  private void startControlReader(final Socket s, final BufferedReader r) {
    Thread t = new Thread(new Runnable() {
      @Override
      public void run() {
        try {
          String line;
          while ((line = r.readLine()) != null) {
            String type = Json.field(line, "type");
            if ("enable".equals(type)) {
              AgentState.setRecording(true);
            } else if ("disable".equals(type)) {
              AgentState.setRecording(false);
              queue.clear();
              queuedBytes.set(0);
            } else if ("detach".equals(type)) {
              // Not on this thread: detach closes the socket it reads from.
              Thread d = new Thread(new Runnable() {
                @Override
                public void run() {
                  AgentState.detach("stopped");
                }
              }, "proj-agent-detach");
              d.setDaemon(true);
              d.start();
              return;
            }
          }
        } catch (IOException e) {
          // connection closed
        }
        if (socket == s) disconnect();
      }
    }, "proj-agent-control");
    t.setDaemon(true);
    t.start();
  }

  private synchronized void disconnect() {
    connected = false;
    if (socket != null) {
      try {
        socket.close();
      } catch (IOException e) {
        // ignore
      }
    }
    socket = null;
  }

  // ---- messages -------------------------------------------------------------

  private void line(String s) throws IOException {
    out.write(s);
    out.write('\n');
  }

  private void periodic() throws IOException {
    long now = System.currentTimeMillis();
    // Long-running traces (a server's main(), a request still running) as snapshots.
    long nanos = System.nanoTime();
    for (TraceTree t : AgentState.open.values()) {
      long age = nanos - t.startNanos;
      if (age > options.snapshotMillis * 1000000L && nanos - t.lastSnapshotNanos > options.snapshotMillis * 1000000L) {
        t.lastSnapshotNanos = nanos;
        writeTree(t, "open");
      }
    }
    if (now - lastStats >= STATS_MILLIS) {
      lastStats = now;
      String s = stats();
      if (!s.equals(lastStatsLine)) {
        line(s);
        lastStatsLine = s;
      }
    }
  }

  private String stats() {
    StringBuilder sb = new StringBuilder("{\"type\":\"stats\",\"session\":\"").append(AgentState.session).append('"');
    sb.append(",\"recording\":").append(Trace.enabled);
    sb.append(",\"since_start\":").append(AgentState.sinceStart);
    sb.append(",\"dropped_items\":").append(AgentState.droppedItems.get());
    sb.append(",\"dropped_bytes\":").append(AgentState.droppedBytes.get());
    sb.append(",\"abandoned_traces\":").append(AgentState.abandonedTraces.get());
    sb.append(",\"open_traces\":").append(AgentState.open.size());
    sb.append(",\"instrumented_classes\":").append(AgentState.instrumentedClasses.get());
    sb.append(",\"instrumented_methods\":").append(AgentState.instrumentedMethods.get());
    sb.append(",\"transform_failures\":").append(AgentState.transformFailures.get());
    sb.append(",\"last_transform_error\":");
    Json.str(sb, AgentState.lastTransformError);
    sb.append(",\"internal_errors\":").append(AgentState.internalErrors.get());
    sb.append(",\"queue_items\":").append(queue.size()).append('}');
    return sb.toString();
  }

  private void write(Item item) throws IOException {
    if (item.tree != null) writeTree(item.tree, item.status);
    else writeUncaught(item);
  }

  private void method(int id) throws IOException {
    if (id < 0 || sentMethods.get(id)) return;
    Methods.Info m = Methods.info(id);
    if (m == null) return;
    StringBuilder sb = new StringBuilder("{\"type\":\"method\",\"id\":").append(id).append(",\"class\":");
    Json.str(sb, m.owner.replace('/', '.')).append(",\"name\":");
    Json.str(sb, m.name).append(",\"desc\":");
    Json.str(sb, m.desc).append(",\"source\":");
    Json.str(sb, m.source);
    sb.append(",\"first_line\":").append(m.firstLine == Integer.MAX_VALUE ? 0 : m.firstLine);
    sb.append(",\"last_line\":").append(m.lastLine).append('}');
    line(sb.toString());
    sentMethods.set(id);
  }

  private void writeTree(TraceTree t, String status) throws IOException {
    int n = t.size;  // volatile read first: the nodes below it are complete
    int[] method = t.method;
    int[] parent = t.parent;
    int[] count = t.count;
    int[] exc = t.exc;
    long[] first = t.first;
    n = Math.min(n, Math.min(Math.min(method.length, parent.length), Math.min(Math.min(count.length, exc.length), first.length)));
    for (int i = 0; i < n; i++) method(method[i]);
    ErrorData[] errors = "open".equals(status) || "incomplete".equals(status) ? null : t.errorData;
    if (errors != null) {
      for (ErrorData e : errors) {
        site(e.throwSite);
        site(e.caughtSite);
        method(e.escapedFrom);
        for (int k = 0; k < e.eventRefs.length; k++) {
          if (e.eventKinds[k] == ErrorRec.EXIT) method(e.eventRefs[k]);
          else site(e.eventRefs[k]);
        }
      }
    }
    StringBuilder sb = new StringBuilder(256 + n * 24);
    sb.append("{\"type\":\"trace\",\"session\":\"").append(AgentState.session).append("\",\"id\":").append(t.id);
    sb.append(",\"status\":\"").append(status).append('"');
    sb.append(",\"thread\":{\"id\":").append(t.threadId).append(",\"name\":");
    Json.str(sb, t.threadName).append("},\"start_ms\":").append(t.startMillis);
    long duration = t.durationNanos >= 0 ? t.durationNanos : System.nanoTime() - t.startNanos;
    sb.append(",\"duration_us\":").append(duration / 1000);
    if (t.httpMethod != null) {
      sb.append(",\"http\":{\"method\":");
      Json.str(sb, t.httpMethod).append(",\"path\":");
      Json.str(sb, t.httpPath).append('}');
    }
    sb.append(",\"nodes\":[");
    for (int i = 0; i < n; i++) {
      if (i > 0) sb.append(',');
      sb.append('[').append(method[i]).append(',').append(parent[i]).append(',').append(count[i]).append(',')
          .append(first[i] / 1000).append(',').append(exc[i]).append(']');
    }
    sb.append("],\"truncated\":").append(t.truncated).append(",\"dropped_nodes\":").append(t.droppedNodes);
    sb.append(",\"dropped_errors\":").append(t.droppedErrors).append(",\"errors\":[");
    if (errors != null) {
      for (int i = 0; i < errors.length; i++) {
        if (i > 0) sb.append(',');
        error(sb, errors[i]);
      }
    }
    sb.append("]}");
    line(sb.toString());
  }

  private void site(int id) throws IOException {
    Methods.Site s = Methods.site(id);
    if (s != null) method(s.method);
  }

  private static void siteJson(StringBuilder sb, int id) {
    Methods.Site s = Methods.site(id);
    if (s == null) {
      sb.append("null");
      return;
    }
    sb.append("{\"method\":").append(s.method).append(",\"line\":").append(s.line);
    if (s.kind != Methods.THROW) {
      sb.append(",\"kind\":\"").append(s.kind == Methods.CATCH ? "catch" : "finally").append("\",\"type\":");
      Json.str(sb, s.type.isEmpty() ? null : s.type.replace('/', '.'));
    }
    sb.append('}');
  }

  private static void frames(StringBuilder sb, ErrorData.Frame[] frames) {
    sb.append('[');
    for (int i = 0; i < frames.length; i++) {
      ErrorData.Frame f = frames[i];
      if (i > 0) sb.append(',');
      sb.append('[');
      Json.str(sb, f.cls).append(',');
      Json.str(sb, f.method).append(',');
      Json.str(sb, f.file).append(',').append(f.line).append(',').append(f.app ? 1 : 0).append(']');
    }
    sb.append(']');
  }

  private static void error(StringBuilder sb, ErrorData e) {
    sb.append("{\"exception\":");
    Json.str(sb, e.type).append(",\"message\":");
    Json.str(sb, e.message).append(",\"state\":\"").append(e.state).append('"');
    sb.append(",\"rethrown\":").append(e.rethrown).append(",\"wrapped_by\":");
    Json.str(sb, e.wrappedBy).append(",\"throw\":");
    siteJson(sb, e.throwSite);
    sb.append(",\"caught\":");
    siteJson(sb, e.caughtSite);
    sb.append(",\"escaped_from\":");
    if (e.escapedFrom >= 0) sb.append(e.escapedFrom);
    else sb.append("null");
    sb.append(",\"frames\":");
    frames(sb, e.frames);
    sb.append(",\"frames_omitted\":").append(e.framesOmitted).append(",\"causes\":[");
    for (int i = 0; i < e.causes.length; i++) {
      ErrorData.Cause c = e.causes[i];
      if (i > 0) sb.append(',');
      sb.append("{\"exception\":");
      Json.str(sb, c.type).append(",\"message\":");
      Json.str(sb, c.message).append(",\"frames\":");
      frames(sb, c.frames);
      sb.append(",\"frames_omitted\":").append(c.framesOmitted).append('}');
    }
    sb.append("],\"suppressed\":").append(e.suppressed).append(",\"events\":[");
    if (e.eventKinds != null) {
      for (int i = 0; i < e.eventKinds.length; i++) {
        if (i > 0) sb.append(',');
        int kind = e.eventKinds[i];
        int ref = e.eventRefs[i];
        if (kind == ErrorRec.EXIT) {
          sb.append("[\"exit\",").append(ref).append(']');
        } else {
          Methods.Site s = Methods.site(ref);
          sb.append("[\"").append(kind == ErrorRec.THROWN ? "thrown" : (s != null && s.kind == Methods.FINALLY ? "finally" : "caught"));
          sb.append("\",").append(s == null ? -1 : s.method).append(',').append(s == null ? 0 : s.line).append(']');
        }
      }
    }
    sb.append("],\"dropped_events\":").append(e.droppedEvents).append('}');
  }

  private void writeUncaught(Item item) throws IOException {
    StringBuilder sb = new StringBuilder("{\"type\":\"uncaught\",\"session\":\"").append(AgentState.session).append('"');
    sb.append(",\"trace\":");
    if (item.trace >= 0) sb.append(item.trace);
    else sb.append("null");
    sb.append(",\"time_ms\":").append(item.time);
    sb.append(",\"thread\":{\"id\":").append(item.threadId).append(",\"name\":");
    Json.str(sb, item.threadName).append("},\"error\":");
    error(sb, item.uncaught);
    sb.append('}');
    line(sb.toString());
  }
}

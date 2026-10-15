package proj.agent.rt;

import java.util.Arrays;

/**
 * Calls observed during one trace, merged by (parent node, method). Node 0 is the trace's root.
 * Built by the owning thread only; the sender reads {@code size} (volatile) first, then the arrays,
 * so a snapshot of an open trace sees complete nodes.
 */
final class TraceTree {
  final long id;
  final long startNanos;
  final long startMillis;
  final long threadId;
  final String threadName;
  final int maxNodes;
  final int maxErrors;

  int[] method;
  int[] parent;
  int[] count;
  int[] exc;     // exits by exception
  long[] first;  // first entry, nanoseconds after the trace started
  volatile int size;
  private long[] keys;  // open addressing: (parent << 32 | method) + 1, 0 = empty
  private int[] values;
  boolean truncated;
  int droppedNodes;

  String httpMethod;
  String httpPath;

  ErrorRec[] errors = new ErrorRec[4];
  int errorCount;
  int droppedErrors;

  // Set when finished (or snapshotted by the sender).
  volatile long durationNanos = -1;
  String status;  // complete|truncated|incomplete
  ErrorData[] errorData;
  long lastSnapshotNanos;

  TraceTree(long id, int rootMethod, Thread thread, int maxNodes, int maxErrors) {
    this.id = id;
    this.startNanos = System.nanoTime();
    this.startMillis = System.currentTimeMillis();
    this.threadId = thread.getId();
    this.threadName = thread.getName();
    this.maxNodes = maxNodes;
    this.maxErrors = maxErrors;
    int cap = 16;
    method = new int[cap];
    parent = new int[cap];
    count = new int[cap];
    exc = new int[cap];
    first = new long[cap];
    keys = new long[32];
    values = new int[32];
    method[0] = rootMethod;
    parent[0] = -1;
    count[0] = 1;
    size = 1;
  }

  /** Node for a call of {@code m} from node {@code p}; -1 when the tree is full. */
  int child(int p, int m) {
    long key = ((long) p << 32 | (m & 0xFFFFFFFFL)) + 1;
    int mask = keys.length - 1;
    int h = mix(key) & mask;
    while (keys[h] != 0) {
      if (keys[h] == key) {
        int n = values[h];
        count[n]++;
        return n;
      }
      h = (h + 1) & mask;
    }
    int n = size;
    if (n >= maxNodes) {
      truncated = true;
      droppedNodes++;
      return -1;
    }
    if (n == method.length) {
      int cap = n * 2;
      method = Arrays.copyOf(method, cap);
      parent = Arrays.copyOf(parent, cap);
      count = Arrays.copyOf(count, cap);
      exc = Arrays.copyOf(exc, cap);
      first = Arrays.copyOf(first, cap);
    }
    method[n] = m;
    parent[n] = p;
    count[n] = 1;
    first[n] = System.nanoTime() - startNanos;
    keys[h] = key;
    values[h] = n;
    size = n + 1;
    if ((n + 1) * 2 > keys.length) rehash();
    return n;
  }

  private static int mix(long k) {
    k ^= k >>> 33;
    k *= 0xff51afd7ed558ccdL;
    k ^= k >>> 33;
    return (int) k;
  }

  private void rehash() {
    long[] oldKeys = keys;
    int[] oldValues = values;
    keys = new long[oldKeys.length * 2];
    values = new int[oldKeys.length * 2];
    int mask = keys.length - 1;
    for (int i = 0; i < oldKeys.length; i++) {
      if (oldKeys[i] == 0) continue;
      int h = mix(oldKeys[i]) & mask;
      while (keys[h] != 0) h = (h + 1) & mask;
      keys[h] = oldKeys[i];
      values[h] = oldValues[i];
    }
  }

  /** The record of this exception object in the trace (identity), created on first sight. */
  ErrorRec error(Throwable ex) {
    for (int i = 0; i < errorCount; i++) {
      if (errors[i].ex == ex) return errors[i];
    }
    if (errorCount >= maxErrors) {
      droppedErrors++;
      return null;
    }
    if (errorCount == errors.length) errors = Arrays.copyOf(errors, errorCount * 2);
    ErrorRec e = new ErrorRec(ex);
    errors[errorCount++] = e;
    return e;
  }
}

package proj.agent.rt;

/** What one exception object did within a trace: thrown, passed out of methods, caught. */
final class ErrorRec {
  static final int THROWN = 0;  // ref: site (an application athrow)
  static final int EXIT = 1;    // ref: method it propagated out of
  static final int CAUGHT = 2;  // ref: site (an application handler)
  private static final int MAX_EVENTS = 32;

  final Throwable ex;
  int[] kinds = new int[8];
  int[] refs = new int[8];
  int count;
  int droppedEvents;
  boolean escaped;  // left the trace's outermost application frame

  ErrorRec(Throwable ex) {
    this.ex = ex;
  }

  void event(int kind, int ref) {
    if (count > 0 && kinds[count - 1] == kind && refs[count - 1] == ref) return;
    if (count == MAX_EVENTS) {
      droppedEvents++;
      return;
    }
    if (count == kinds.length) {
      kinds = java.util.Arrays.copyOf(kinds, count * 2);
      refs = java.util.Arrays.copyOf(refs, count * 2);
    }
    kinds[count] = kind;
    refs[count] = ref;
    count++;
  }
}

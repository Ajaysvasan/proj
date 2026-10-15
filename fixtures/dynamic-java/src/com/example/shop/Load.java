package com.example.shop;

import com.example.shop.service.UserService;

/** Benchmarks: the user lookup path in a tight loop (overhead), or as many short traces (throughput). */
public class Load {
  public static void run(int seconds) {
    UserService users = new UserService();
    long end = System.nanoTime() + seconds * 1_000_000_000L;
    long calls = 0;
    long checksum = 0;
    while (System.nanoTime() < end) {
      for (int i = 0; i < 1000; i++) {
        checksum += users.describe(i % 50).length();
        calls++;
      }
    }
    System.out.println("{\"calls\":" + calls + ",\"seconds\":" + seconds + ",\"checksum\":" + checksum + "}");
  }

  /** {@code count} lookups, each its own trace: a task run by a pool thread. */
  public static void traces(int count) throws InterruptedException {
    final UserService users = new UserService();
    java.util.concurrent.ExecutorService pool = java.util.concurrent.Executors.newFixedThreadPool(4);
    long start = System.nanoTime();
    for (int i = 0; i < count; i++) pool.execute(new Lookup(users, i));
    pool.shutdown();
    pool.awaitTermination(10, java.util.concurrent.TimeUnit.MINUTES);
    long ms = (System.nanoTime() - start) / 1_000_000;
    System.out.println("{\"traces\":" + count + ",\"ms\":" + ms + "}");
  }

  static final class Lookup implements Runnable {
    private final UserService users;
    private final int id;

    Lookup(UserService users, int id) {
      this.users = users;
      this.id = id;
    }

    public void run() {
      users.describe(id % 50);
    }
  }
}

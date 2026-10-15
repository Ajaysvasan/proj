package com.example.shop;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/** Asynchronous work: tasks run on their own thread. */
public class Background {
  private static final ExecutorService POOL = Executors.newSingleThreadExecutor();

  public static void submit(Runnable task) {
    POOL.submit(task);
  }

  /** A callback that fails inside library code, which keeps the failure to itself. */
  public static boolean probe() {
    return CompletableFuture.completedFuture("x").thenApply(Background::check).isCompletedExceptionally();
  }

  static String check(String value) {
    throw new IllegalArgumentException("probe rejected " + value);
  }

  public static void shutdown() throws InterruptedException {
    POOL.shutdown();
    POOL.awaitTermination(5, TimeUnit.SECONDS);
  }
}

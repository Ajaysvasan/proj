package com.example.shop;

/** A thread that ends with an uncaught exception. */
public class Crasher implements Runnable {
  public static void start() throws InterruptedException {
    Thread t = new Thread(new Crasher(), "crasher");
    t.start();
    t.join();
  }

  public void run() {
    explode();
  }

  private void explode() {
    throw new IllegalStateException("crasher gave up");
  }
}

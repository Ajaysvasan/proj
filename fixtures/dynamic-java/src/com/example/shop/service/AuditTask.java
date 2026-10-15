package com.example.shop.service;

public class AuditTask implements Runnable {
  private final String item;

  public AuditTask(String item) {
    this.item = item;
  }

  public void run() {
    record(item);
  }

  private void record(String what) {
    if (what.isEmpty()) throw new IllegalArgumentException("nothing to audit");
  }
}

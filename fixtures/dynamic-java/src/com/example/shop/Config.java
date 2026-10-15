package com.example.shop;

public class Config {
  private final int cacheSize;

  private Config(int cacheSize) {
    this.cacheSize = cacheSize;
  }

  public static Config load() {
    return new Config(parse("16"));
  }

  private static int parse(String value) {
    return Integer.parseInt(value);
  }

  public int cacheSize() {
    return cacheSize;
  }

  /** As if read from a properties or XML file. */
  public String pluginClass() {
    return "com.example.shop.plugins.AuditPlugin";
  }
}

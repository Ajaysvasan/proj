package com.example.shop;

import com.example.shop.repo.UserRepository;

/** Startup hook: runs once before the server accepts requests. */
public class Bootstrap {
  public static void init() throws Exception {
    Config config = Config.load();
    UserRepository.warmUp(config.cacheSize());
    // A plugin named in the configuration, started by reflection.
    Object plugin = Class.forName(config.pluginClass()).getDeclaredConstructor().newInstance();
    plugin.getClass().getMethod("start").invoke(plugin);
  }
}

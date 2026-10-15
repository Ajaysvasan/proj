package com.example.shop.repo;

import com.example.shop.User;

public class UserRepository {
  private static int cache;

  public static void warmUp(int size) {
    cache = size;
  }

  public User load(long id) {
    return new User(id, "user" + (id % (cache == 0 ? 1 : cache)));
  }
}

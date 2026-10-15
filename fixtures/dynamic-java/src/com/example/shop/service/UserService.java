package com.example.shop.service;

import com.example.shop.User;
import com.example.shop.repo.UserRepository;

public class UserService {
  private final UserRepository repo = new UserRepository();

  public String describe(long id) {
    User user = repo.load(id);
    String text = id % 2 == 0 ? format(user) : format(user, true);
    return text + " depth=" + depth(3);
  }

  // Overloads: the runtime tells which one ran.
  String format(User user) {
    return user.name;
  }

  String format(User user, boolean withId) {
    return withId ? user.id + ":" + user.name : format(user);
  }

  int depth(int n) {
    return n == 0 ? 0 : 1 + depth(n - 1);
  }
}

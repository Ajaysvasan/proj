package com.example.shop.web;

import com.example.shop.service.UserService;

public class UserHandler extends BaseHandler {
  private final UserService users = new UserService();

  protected String doHandle(String path, String query) {
    long id;
    try {
      id = Long.parseLong(path.substring(path.lastIndexOf('/') + 1));
    } catch (NumberFormatException e) {
      id = 0;  // caught: an unknown id is user 0
    }
    return users.describe(id);
  }
}

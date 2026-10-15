package com.example.shop.web;

import com.example.shop.service.OrderService;

public class OrderHandler extends BaseHandler {
  private final OrderService orders = new OrderService();

  protected String doHandle(String path, String query) {
    try {
      return orders.create(query == null ? "" : query.substring(query.indexOf('=') + 1));
    } catch (IllegalStateException e) {
      log(e);
      throw e;  // rethrown: escapes the handler
    }
  }

  private void log(Exception e) {
    System.err.println("order failed: " + e.getMessage());
  }
}

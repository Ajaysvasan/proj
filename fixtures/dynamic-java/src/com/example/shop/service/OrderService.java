package com.example.shop.service;

import com.example.shop.Background;
import com.example.shop.repo.PaymentRepository;
import java.sql.SQLException;

public class OrderService {
  private final PaymentRepository payments = new PaymentRepository();

  public String create(String item) {
    try {
      payments.save(item);
    } catch (SQLException e) {
      throw new IllegalStateException("payment failed for '" + item + "'", e);  // wraps the cause
    }
    Background.submit(new AuditTask(item));
    return "ordered " + item;
  }
}

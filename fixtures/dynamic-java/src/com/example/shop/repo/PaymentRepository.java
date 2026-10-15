package com.example.shop.repo;

import java.sql.SQLException;

public class PaymentRepository {
  public void save(String item) throws SQLException {
    if (item.isEmpty()) {
      throw new SQLException("empty item");  // thrown in application code
    }
  }
}

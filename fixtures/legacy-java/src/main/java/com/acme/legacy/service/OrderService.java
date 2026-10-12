package com.acme.legacy.service;

import java.util.ArrayList;
import java.util.List;

public class OrderService {
    public List<String> list() {
        return new ArrayList<>();
    }

    public void place(String sku, int quantity) {}

    public void place(String sku, long quantity) {}
}

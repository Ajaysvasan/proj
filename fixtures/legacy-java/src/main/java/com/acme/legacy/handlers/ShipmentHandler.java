package com.acme.legacy.handlers;

import java.util.Map;

public class ShipmentHandler {
    public String doGet(Map<String, String> request) {
        return "shipments";
    }

    public String doPost(Map<String, String> request) {
        return "created";
    }
}

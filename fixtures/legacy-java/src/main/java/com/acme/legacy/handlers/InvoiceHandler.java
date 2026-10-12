package com.acme.legacy.handlers;

import java.util.Map;

/** Gateway handler (routes.yaml): the gateway calls handle() unless a method is named. */
public class InvoiceHandler {
    public String handle(Map<String, String> request) {
        return InvoiceStore.all().toString();
    }

    public String update(String id, String body) {
        InvoiceStore.save(id, body);
        return id;
    }
}

package com.acme.legacy.server;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;

public class StatusHandler implements HttpHandler {
    @Override
    public void handle(HttpExchange exchange) {
        StatusService.collect();
    }
}

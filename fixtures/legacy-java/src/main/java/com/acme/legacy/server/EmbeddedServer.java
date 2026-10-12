package com.acme.legacy.server;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;

/** A plain JDK HTTP server: no framework, no annotations. */
public class EmbeddedServer {
    public static void main(String[] args) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress(8081), 0);
        server.createContext("/status", new StatusHandler());
        server.createContext("/metrics", exchange -> MetricsReporter.report(exchange));
        server.createContext("/echo", new HttpHandler() {
            @Override
            public void handle(HttpExchange exchange) throws IOException {
                EchoService.echo(exchange);
            }
        });
        StatusHandler shared = new StatusHandler();
        server.createContext("/status/shared", shared);
        server.start();
    }
}

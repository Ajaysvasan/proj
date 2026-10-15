package com.example.shop.web;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpHandler;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/** Template handler: subclasses implement doHandle; the response is always closed. */
public abstract class BaseHandler implements HttpHandler {
  public void handle(HttpExchange exchange) throws IOException {
    try {
      String body = doHandle(exchange.getRequestURI().getPath(), exchange.getRequestURI().getQuery());
      respond(exchange, 200, body);
    } finally {
      exchange.close();
    }
  }

  protected abstract String doHandle(String path, String query) throws IOException;

  private void respond(HttpExchange exchange, int status, String body) throws IOException {
    byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
    exchange.sendResponseHeaders(status, bytes.length);
    OutputStream out = exchange.getResponseBody();
    out.write(bytes);
    out.close();
  }
}

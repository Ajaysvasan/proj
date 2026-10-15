package com.example.shop;

import com.example.shop.web.OrderHandler;
import com.example.shop.web.UserHandler;
import com.sun.net.httpserver.HttpServer;
import java.io.File;
import java.io.FileOutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Controlled application for runtime tracing tests.
 *
 *   ShopApp scenario <port-file>   start, run a fixed set of requests (some concurrent), exit
 *   ShopApp wait <port-file>       start and serve until killed
 *   ShopApp load <seconds>         no server: call the request path in a loop (overhead benchmark)
 *   ShopApp traces <count>         no server: that many short traces on a thread pool (throughput)
 */
public class ShopApp {
  public static void main(String[] args) throws Exception {
    String mode = args.length > 0 ? args[0] : "scenario";
    if (mode.equals("load")) {
      Load.run(Integer.parseInt(args[1]));
      return;
    }
    if (mode.equals("traces")) {
      Load.traces(Integer.parseInt(args[1]));
      return;
    }
    Bootstrap.init();
    Background.probe();
    HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 16);
    ExecutorService workers = Executors.newFixedThreadPool(4);
    server.setExecutor(workers);
    server.createContext("/users/", new UserHandler());
    server.createContext("/orders", new OrderHandler());
    server.start();
    int port = server.getAddress().getPort();
    writePort(args.length > 1 ? args[1] : null, port);
    if (mode.equals("wait")) {
      Thread.sleep(Long.MAX_VALUE);
    }
    ScenarioClient.run(port);
    Crasher.start();
    server.stop(0);
    workers.shutdown();
    Background.shutdown();
  }

  private static void writePort(String file, int port) throws Exception {
    if (file == null) return;
    File tmp = new File(file + ".tmp");
    FileOutputStream out = new FileOutputStream(tmp);
    out.write(Integer.toString(port).getBytes(StandardCharsets.UTF_8));
    out.close();
    if (!tmp.renameTo(new File(file))) throw new IllegalStateException("cannot write " + file);
  }
}

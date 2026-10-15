package com.example.shop;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;

/** The fixed requests of the scenario mode: four concurrent user lookups, then two orders. */
public class ScenarioClient {
  public static void run(int port) throws Exception {
    List<Thread> threads = new ArrayList<Thread>();
    for (final String id : new String[] {"1", "2", "3", "x"}) {
      Thread t = new Thread(new Runnable() {
        public void run() {
          get(port, "/users/" + id);
        }
      }, "client-" + id);
      threads.add(t);
      t.start();
    }
    for (Thread t : threads) t.join();
    get(port, "/orders?item=book");
    get(port, "/orders?item=");
  }

  static int get(int port, String path) {
    try {
      HttpURLConnection c = (HttpURLConnection) new URL("http://127.0.0.1:" + port + path).openConnection();
      int status = c.getResponseCode();
      InputStream in = status < 400 ? c.getInputStream() : c.getErrorStream();
      if (in != null) {
        while (in.read() >= 0) {}
        in.close();
      }
      c.disconnect();
      return status;
    } catch (Exception e) {
      return -1;
    }
  }
}

package com.acme.legacy.server;

import com.acme.legacy.web.OrderServlet;
import com.acme.legacy.web.UserServlet;
import org.eclipse.jetty.server.Server;
import org.eclipse.jetty.servlet.ServletContextHandler;
import org.eclipse.jetty.servlet.ServletHolder;

public class JettyLauncher {
    public static void main(String[] args) throws Exception {
        Server server = new Server(8082);
        ServletContextHandler context = new ServletContextHandler();
        context.addServlet(new ServletHolder(new UserServlet()), "/v1/users/*");
        context.addServlet(ExportServlet.class, "/v1/export-all");
        ServletHolder orders = new ServletHolder(OrderServlet.class);
        context.addServlet(orders, "/v1/orders/*");
        server.setHandler(context);
        server.start();
    }
}

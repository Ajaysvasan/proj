package com.acme.legacy.web;

import com.acme.legacy.service.OrderService;
import java.io.IOException;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

/** Handles every HTTP method itself. */
public class OrderServlet extends HttpServlet {
    private final OrderService orders = new OrderService();

    @Override
    protected void service(HttpServletRequest req, HttpServletResponse resp) throws IOException {
        if ("POST".equals(req.getMethod())) {
            orders.place(req.getParameter("sku"), 1);
        } else {
            resp.getWriter().write(orders.list().toString());
        }
    }
}

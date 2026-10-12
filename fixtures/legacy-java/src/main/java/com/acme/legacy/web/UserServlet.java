package com.acme.legacy.web;

import com.acme.legacy.service.UserService;
import java.io.IOException;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

public class UserServlet extends BaseServlet {
    private final UserService users = new UserService();

    @Override
    protected void doGet(HttpServletRequest req, HttpServletResponse resp) throws IOException {
        String id = req.getParameter("id");
        if (id == null) {
            write(resp, users.find("all", 50));
        } else {
            write(resp, users.find(42L));
            write(resp, users.find("bob"));
        }
    }

    @Override
    protected void doPost(HttpServletRequest req, HttpServletResponse resp) throws IOException {
        users.save(req.getParameter("name"));
        resp.setStatus(201);
    }

    private void write(HttpServletResponse resp, Object body) throws IOException {
        resp.getWriter().write(String.valueOf(body));
    }
}

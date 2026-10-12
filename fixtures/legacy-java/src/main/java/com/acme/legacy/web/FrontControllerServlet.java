package com.acme.legacy.web;

import com.acme.legacy.service.OrderService;
import com.acme.legacy.service.UserService;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

/** One servlet for the whole legacy app: it dispatches on the path itself. */
public class FrontControllerServlet extends HttpServlet {
    private final Map<String, Command> commandMap = new HashMap<>();
    private final UserService users = new UserService();

    @Override
    public void init() {
        commandMap.put("/cmd/login", new LoginCommand());
        commandMap.put("/cmd/logout", new LogoutCommand());
    }

    @Override
    protected void doGet(HttpServletRequest req, HttpServletResponse resp) throws IOException {
        String path = req.getPathInfo();
        if ("/profile".equals(path)) {
            showProfile(req, resp);
        } else if (path.startsWith("/orders")) {
            new OrderService().list();
        }
        switch (req.getServletPath()) {
            case "/users":
                users.find("all", 10);
                break;
            case "/help":
                renderHelp(resp);
                break;
            default:
                resp.setStatus(404);
        }
    }

    private void showProfile(HttpServletRequest req, HttpServletResponse resp) throws IOException {
        resp.getWriter().write(users.find(req.getParameter("name")));
    }

    private void renderHelp(HttpServletResponse resp) throws IOException {
        resp.getWriter().write("help");
    }
}

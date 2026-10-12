package com.acme.legacy.web;

import java.io.IOException;
import javax.servlet.ServletException;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

/** Shared behavior for the portal's servlets. */
public abstract class BaseServlet extends HttpServlet {
    @Override
    protected void doDelete(HttpServletRequest req, HttpServletResponse resp) throws ServletException, IOException {
        audit("delete", req.getPathInfo());
        resp.setStatus(405);
    }

    protected void audit(String action, String path) {
        AuditLog.record(action, path);
    }
}

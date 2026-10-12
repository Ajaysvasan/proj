package com.acme.legacy.web;

import javax.servlet.http.HttpServlet;

/** Declared in web.xml but never overrides a request method. */
public class HealthServlet extends HttpServlet {
    public String ping() {
        return "ok";
    }
}

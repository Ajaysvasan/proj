package com.acme.legacy.web;

import java.io.IOException;
import javax.servlet.Filter;
import javax.servlet.FilterChain;
import javax.servlet.FilterConfig;
import javax.servlet.ServletException;
import javax.servlet.ServletRequest;
import javax.servlet.ServletResponse;

public class AuthFilter implements Filter {
    @Override
    public void init(FilterConfig config) {}

    @Override
    public void doFilter(ServletRequest req, ServletResponse resp, FilterChain chain) throws IOException, ServletException {
        if (!TokenValidator.valid(req.getParameter("token"))) {
            throw new ServletException("unauthorized");
        }
        chain.doFilter(req, resp);
    }

    @Override
    public void destroy() {}
}

package com.acme.legacy.web;

import javax.servlet.ServletContextEvent;
import javax.servlet.ServletContextListener;

public class AppListener implements ServletContextListener {
    @Override
    public void contextInitialized(ServletContextEvent event) {
        Bootstrap.start();
    }

    @Override
    public void contextDestroyed(ServletContextEvent event) {}
}

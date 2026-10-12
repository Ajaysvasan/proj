package com.acme.legacy.server;

import java.util.Set;
import javax.servlet.ServletContainerInitializer;
import javax.servlet.ServletContext;
import javax.servlet.ServletRegistration;

/** Programmatic servlet registration (Servlet 3.0). */
public class AppInitializer implements ServletContainerInitializer {
    @Override
    public void onStartup(Set<Class<?>> classes, ServletContext ctx) {
        ServletRegistration.Dynamic reports = ctx.addServlet("reports", new ReportServlet());
        reports.addMapping("/v1/reports/*");
        ctx.addServlet("audit", AuditServlet.class).addMapping("/v1/audit", "/v1/audit/*");
    }
}

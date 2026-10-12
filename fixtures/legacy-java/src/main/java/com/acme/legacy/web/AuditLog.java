package com.acme.legacy.web;

public final class AuditLog {
    private AuditLog() {}

    public static void record(String action, String path) {
        System.out.println(action + " " + path);
    }
}

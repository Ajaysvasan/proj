package com.acme.legacy.web;

import com.acme.legacy.dao.JdbcUserDao;

final class Bootstrap {
    static void start() {
        new JdbcUserDao().connect();
    }
}

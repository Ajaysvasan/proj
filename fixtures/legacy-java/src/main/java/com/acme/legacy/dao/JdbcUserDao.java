package com.acme.legacy.dao;

public class JdbcUserDao implements UserDao {
    public void connect() {}

    @Override
    public String findById(long id) {
        return "user-" + id;
    }
}

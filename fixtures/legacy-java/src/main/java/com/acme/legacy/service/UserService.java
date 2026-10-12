package com.acme.legacy.service;

import com.acme.legacy.dao.UserDao;
import java.util.List;

/** Many overloads, as in the legacy code base. */
public class UserService {
    private UserDao userDao;

    public void setUserDao(UserDao userDao) {
        this.userDao = userDao;
    }

    public String find(long id) {
        return userDao.findById(id);
    }

    public String find(String name) {
        return "user " + name;
    }

    public List<String> find(String query, int limit) {
        return List.of(query);
    }

    public void save(String name) {}

    public boolean authenticate(String user, String password) {
        return user != null && password != null;
    }
}

package com.example.demo.repository;

import com.example.demo.model.User;
import java.util.HashMap;
import java.util.Map;

public class UserRepository {
    private final Map<Long, User> users = new HashMap<>();

    public User findById(Long id) {
        return users.get(id);
    }

    public User save(User user) {
        users.put(user.getId(), user);
        return user;
    }

    public void delete(User user) {
        users.remove(user.getId());
    }
}

package com.example.demo.service;

import com.example.demo.model.User;

public interface UserService {
    User findUser(Long id);

    User register(User user);

    void remove(Long id);
}

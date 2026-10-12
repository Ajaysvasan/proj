package com.example.demo.service;

import com.example.demo.error.UserNotFoundException;
import com.example.demo.model.User;
import com.example.demo.repository.UserRepository;
import org.springframework.stereotype.Service;

@Service
public class UserServiceImpl implements UserService {
    private final UserRepository repository;

    public UserServiceImpl(UserRepository repository) {
        this.repository = repository;
    }

    @Override
    public User findUser(Long id) {
        User user = repository.findById(id);
        if (user == null) {
            throw new UserNotFoundException(id);
        }
        return user;
    }

    @Override
    public User register(User user) {
        validate(user);
        return repository.save(user);
    }

    @Override
    public void remove(Long id) throws UserNotFoundException {
        repository.delete(findUser(id));
    }

    private void validate(User user) {
        validate(user, false);
    }

    private void validate(User user, boolean strict) {
        if (user.getEmail() == null) {
            throw new IllegalArgumentException("email is required");
        }
    }
}

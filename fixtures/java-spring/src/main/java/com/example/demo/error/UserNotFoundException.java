package com.example.demo.error;

public class UserNotFoundException extends RuntimeException {
    public UserNotFoundException(Long id) {
        super("user " + id + " not found");
    }
}

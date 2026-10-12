package com.example.demo.web;

import com.example.demo.error.UserNotFoundException;
import com.example.demo.model.User;
import com.example.demo.service.UserService;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

@RestController
@RequestMapping("/api/users")
public class UserController {
    private final UserService userService;

    public UserController(UserService userService) {
        this.userService = userService;
    }

    @GetMapping("/{id}")
    public ResponseEntity<User> getUser(@PathVariable Long id) {
        try {
            return ResponseEntity.ok(userService.findUser(id));
        } catch (UserNotFoundException e) {
            return ResponseEntity.notFound().build();
        }
    }

    @PostMapping
    public User createUser(@RequestBody User user) {
        return userService.register(user);
    }

    @RequestMapping(value = "/{id}", method = RequestMethod.DELETE)
    public void deleteUser(@PathVariable Long id) {
        userService.remove(id);
    }
}

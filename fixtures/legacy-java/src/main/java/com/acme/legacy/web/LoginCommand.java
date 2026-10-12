package com.acme.legacy.web;

public class LoginCommand implements Command {
    @Override
    public String execute(String input) {
        return "login " + input;
    }
}

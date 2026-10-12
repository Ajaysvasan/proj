package com.acme.legacy.web;

public class LogoutCommand implements Command {
    @Override
    public String execute(String input) {
        return "bye";
    }
}

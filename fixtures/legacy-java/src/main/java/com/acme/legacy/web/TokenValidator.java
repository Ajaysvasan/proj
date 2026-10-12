package com.acme.legacy.web;

final class TokenValidator {
    static boolean valid(String token) {
        return token != null && !token.isEmpty();
    }
}

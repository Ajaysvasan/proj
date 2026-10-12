package com.acme.legacy.api;

public class CustomerResource implements CustomerApi {
    @Override
    public String getCustomer(String id) {
        return "customer " + id;
    }

    @Override
    public String createCustomer(String body) {
        return body;
    }
}

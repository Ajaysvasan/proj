package com.acme.legacy.server;

import spark.Request;
import spark.Response;

final class UserRoutes {
    static Object show(Request req, Response res) {
        return req.params(":id");
    }

    static Object create(Request req, Response res) {
        return "created";
    }
}

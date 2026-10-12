package com.acme.legacy.server;

import static spark.Spark.get;
import static spark.Spark.post;

public class SparkApp {
    public static void main(String[] args) {
        get("/spark/hello", (req, res) -> Greeter.greet(req.queryParams("name")));
        get("/spark/users/:id", UserRoutes::show);
        post("/spark/users", UserRoutes::create);
    }
}

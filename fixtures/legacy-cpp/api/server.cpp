#include <httplib.h>

#include <memory>
#include <string>

#include "reports.hpp"

namespace api {

class Users {
 public:
  void list(const httplib::Request& req, httplib::Response& res);
  void create(const httplib::Request& req, httplib::Response& res);
};

void Users::list(const httplib::Request& req, httplib::Response& res) { res.set_content("[]", "application/json"); }
void Users::create(const httplib::Request& req, httplib::Response& res) { res.status = 201; }

void ping(const httplib::Request& req, httplib::Response& res) { res.set_content("pong", "text/plain"); }

void run() {
  httplib::Server svr;
  Users users;
  svr.Get("/ping", ping);
  svr.Get("/users", [&](const httplib::Request& req, httplib::Response& res) { users.list(req, res); });
  svr.Post("/users", std::bind(&Users::create, &users, std::placeholders::_1, std::placeholders::_2));
  svr.listen("0.0.0.0", 8080);
}

}  // namespace api

int main() {
  api::run();
  return 0;
}

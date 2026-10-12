#pragma once

#include <string>
#include <vector>

namespace store {

struct User {
  int id;
  std::string name;
};

class UserStore {
 public:
  UserStore();
  const User& find(int id) const;
  void add(const User& user);

 private:
  std::vector<User> users_;
};

}  // namespace store

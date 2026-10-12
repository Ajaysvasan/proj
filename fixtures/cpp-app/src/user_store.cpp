#include "store/user_store.h"

#include <stdexcept>

namespace store {

UserStore::UserStore() = default;

const User& UserStore::find(int id) const {
  for (const auto& u : users_) {
    if (u.id == id) return u;
  }
  throw std::out_of_range("user not found");
}

void UserStore::add(const User& user) {
  users_.push_back(user);
}

}  // namespace store

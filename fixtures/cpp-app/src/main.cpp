#include <stdexcept>

#include "store/user_store.h"
#include "util.h"

int main() {
  store::UserStore users;
  users.add(store::User{1, "ada"});
  try {
    const auto& u = users.find(1);
    print_user(u);
  } catch (const std::out_of_range& e) {
    log_error(e.what());
    return 1;
  }
  return 0;
}

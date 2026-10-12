#pragma once

#include <string>
#include <vector>

#include "db/sqlite.hpp"

namespace proj::test {

// Canonical, row-id-independent text form of an index: one sorted line per
// fact, with symbols identified by file path, qualified name and position.
// Two indexes describing the same source tree produce identical dumps whether
// they were built by a full analysis or by a refresh.
std::vector<std::string> canonical_dump(db::Database& db);

}  // namespace proj::test

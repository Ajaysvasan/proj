// proj_index_dump <index.db>: prints the canonical dump of an index (see
// index_dump.hpp). Used to compare a refreshed index with a full analysis:
//   diff <(proj_index_dump a.db) <(proj_index_dump b.db)
#include <cstdio>
#include <exception>

#include "db/sqlite.hpp"
#include "index_dump.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: proj_index_dump <index.db>\n");
    return 2;
  }
  try {
    proj::db::Database db(argv[1], proj::db::OpenMode::ReadOnly);
    for (const auto& line : proj::test::canonical_dump(db)) std::printf("%s\n", line.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "proj_index_dump: %s\n", e.what());
    return 1;
  }
  return 0;
}

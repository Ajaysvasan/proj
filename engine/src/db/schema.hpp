#pragma once

#include <stdexcept>
#include <string>

#include "db/sqlite.hpp"

namespace proj::db {

// Raised when an index or registry database exists but cannot be used by this
// version of the tool (corrupt, missing metadata, or a newer schema).
class SchemaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Connection settings for the build of a fresh index file. The file is
// private to the running analysis until it is atomically renamed into place.
void configure_index_build(Database& db);

// Connection settings for reading a finished index.
void configure_index_read(Database& db);

// Creates every index table and index in an empty database.
void create_index_schema(Database& db);

// Validates that `db` is an index this tool can read. Throws SchemaError with
// an actionable message otherwise. Returns the schema version.
int check_index_schema(Database& db);

// Creates or upgrades the registry schema in place. Existing data is kept.
// Throws SchemaError if the registry was written by a newer tool version.
void migrate_registry(Database& db);

int registry_schema_version(Database& db);

}  // namespace proj::db

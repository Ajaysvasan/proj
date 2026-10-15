#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The dynamic tracing service (`proj-engine --dynamic-service`): a single-threaded process that
// waits for the Java agents of monitored projects, records what they observe into each project's
// runtime store, attaches the agent to running JVMs when asked, and answers `proj start|stop|status
// dynamic`. It listens on 127.0.0.1 only; every connection must present the random token from the
// service file, which only its owner can read.
namespace proj::dynamic {

struct ServiceOptions {
  std::filesystem::path data_dir;
  std::filesystem::path agent_jar;
  std::string java = "java";  // JDK used to attach (needs the attach API)
  int discovery_ms = 2000;    // match rules: how often to look for new JVMs while idle
};

std::filesystem::path service_dir(const std::filesystem::path& data_dir);
std::filesystem::path service_file(const std::filesystem::path& data_dir);
std::filesystem::path runtime_store_path(const std::filesystem::path& data_dir, int64_t project_id);

// Java packages of a project's indexed sources, and the prefixes the agent instruments: packages
// grouped by their first two segments, each group reduced to its common prefix.
std::vector<std::string> java_packages(const std::filesystem::path& index_path);
std::vector<std::string> include_roots(std::vector<std::string> packages);

int run_service(const ServiceOptions& options);

}  // namespace proj::dynamic

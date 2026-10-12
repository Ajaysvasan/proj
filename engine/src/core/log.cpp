#include "core/log.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>

#include "core/json.hpp"
#include "core/sysinfo.hpp"
#include "core/time.hpp"

namespace proj::log {
namespace {

constexpr std::uintmax_t kMaxLogBytes = 1024 * 1024;
constexpr int kKeepBackups = 3;

struct State {
  Level stderr_threshold = Level::Warn;
  std::optional<std::filesystem::path> file;
  std::mutex mu;
};

State& state() {
  static State s;
  return s;
}

}  // namespace

std::optional<Level> parse_level(std::string_view name) {
  if (name == "debug") return Level::Debug;
  if (name == "info") return Level::Info;
  if (name == "warn" || name == "warning") return Level::Warn;
  if (name == "error") return Level::Error;
  return std::nullopt;
}

const char* level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
  }
  return "INFO";
}

void configure(Level stderr_threshold, std::optional<std::filesystem::path> file) {
  auto& s = state();
  std::lock_guard lock(s.mu);
  s.stderr_threshold = stderr_threshold;
  s.file = std::move(file);
}

void rotate_if_needed(const std::filesystem::path& file, std::uintmax_t max_bytes, int keep) {
  std::error_code ec;
  auto size = std::filesystem::file_size(file, ec);
  if (ec || size < max_bytes) return;
  for (int i = keep - 1; i >= 1; --i) {
    auto from = file.string() + "." + std::to_string(i);
    auto to = file.string() + "." + std::to_string(i + 1);
    std::filesystem::rename(from, to, ec);  // missing backups are fine
  }
  std::filesystem::rename(file, file.string() + ".1", ec);
}

void write(Level level, std::string_view component, std::string_view message) {
  auto& s = state();
  std::lock_guard lock(s.mu);
  std::string ts = iso_now();
  if (level >= s.stderr_threshold) {
    std::fprintf(stderr, "%s %s [%.*s] %.*s\n", ts.c_str(), level_name(level),
                 static_cast<int>(component.size()), component.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
  }
  if (s.file && level >= Level::Info) {
    std::error_code ec;
    std::filesystem::create_directories(s.file->parent_path(), ec);
    rotate_if_needed(*s.file, kMaxLogBytes, kKeepBackups);
    std::ofstream out(*s.file, std::ios::app);
    if (out) {
      json line = {{"ts", ts},
                   {"level", level_name(level)},
                   {"source", "engine"},
                   {"pid", current_pid()},
                   {"component", std::string(component)},
                   {"message", std::string(message)}};
      out << line.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    }
  }
}

}  // namespace proj::log

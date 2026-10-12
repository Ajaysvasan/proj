#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/json.hpp"

namespace proj {
class IndexReader;
class Registry;
}  // namespace proj

// JSON protocol between the CLI and the engine. See docs/ARCHITECTURE.md.
//
// Request:  {"protocol":1, "id":<any>, "op":"<name>", "args":{...},
//            "options":{"data_dir":"...", "log_level":"warn"}}
// Response: {"protocol":1, "id":<same>, "op":"<name>", "status":"<status>",
//            "result":{...}|null, "diagnostics":[...], "error":{...}|null}
namespace proj::protocol {

namespace status {
inline constexpr const char* kOk = "ok";
inline constexpr const char* kPartial = "partial";
inline constexpr const char* kInvalidArguments = "invalid_arguments";
inline constexpr const char* kUnsupportedOperation = "unsupported_operation";
inline constexpr const char* kNotFound = "not_found";
inline constexpr const char* kConflict = "conflict";
inline constexpr const char* kIndexError = "index_error";
inline constexpr const char* kAnalysisFailed = "analysis_failed";
inline constexpr const char* kInternalError = "internal_error";
}  // namespace status

// Thrown by operation handlers to produce a non-ok response.
class ProtocolError : public std::runtime_error {
 public:
  ProtocolError(std::string status, std::string code, const std::string& message, json details = nullptr)
      : std::runtime_error(message), status_(std::move(status)), code_(std::move(code)), details_(std::move(details)) {}
  const std::string& status() const { return status_; }
  const std::string& code() const { return code_; }
  const json& details() const { return details_; }

 private:
  std::string status_;
  std::string code_;
  json details_;
};

// Dispatches protocol requests. In --serve mode one Engine instance handles
// many requests and keeps read-only index connections open between them.
class Engine {
 public:
  Engine();
  ~Engine();

  // Never throws: every failure becomes a response envelope.
  json handle(const json& request);
  // Parses a raw request line first; malformed JSON yields invalid_arguments.
  json handle_text(const std::string& line);

 private:
  struct CachedIndex;
  std::map<std::string, std::unique_ptr<CachedIndex>> indexes_;
  std::unique_ptr<Registry> registry_;
  std::filesystem::path registry_dir_;

  Registry& registry(const std::filesystem::path& data_dir);
  IndexReader& open_index(const std::filesystem::path& data_dir, const json& args);
  json dispatch(const std::string& op, const json& args, const std::filesystem::path& data_dir, json& diagnostics,
                std::string& status);
};

json make_response(const json& id, const std::string& op, const std::string& status, json result, json diagnostics,
                   json error);

}  // namespace proj::protocol

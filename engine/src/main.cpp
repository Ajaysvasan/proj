// proj-engine: the native analysis engine behind the `proj` CLI.
//
//   proj-engine                  read one JSON request from stdin, write one response
//   proj-engine --serve          newline-delimited requests/responses until EOF
//   proj-engine --version        print version information as JSON
//   proj-engine --dump-ast LANG FILE   print a Tree-sitter syntax tree (development aid)
//
// stdout carries only protocol JSON; all diagnostics go to stderr.
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "core/json.hpp"
#include "core/version.hpp"
#include "parse/parser.hpp"
#include "protocol/protocol.hpp"
#include "scan/language.hpp"

namespace {

constexpr size_t kMaxRequestBytes = 4 * 1024 * 1024;

void write_line(const proj::json& response) {
  std::string out = response.dump(-1, ' ', false, proj::json::error_handler_t::replace);
  out += '\n';
  std::fwrite(out.data(), 1, out.size(), stdout);
  std::fflush(stdout);
}

int usage() {
  std::fprintf(stderr,
               "usage: proj-engine [--serve | --version | --dump-ast <language> <file>]\n"
               "Reads JSON protocol requests on stdin; see docs/ARCHITECTURE.md.\n");
  return 2;
}

int dump_ast(const std::string& lang_name, const std::string& file) {
  auto lang = proj::language_from_name(lang_name);
  if (!lang) {
    std::fprintf(stderr, "unknown language '%s'\n", lang_name.c_str());
    return 2;
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", file.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  proj::ParserManager parser;
  std::cout << parser.dump(*lang, ss.str()) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);  // a closed stdout surfaces as a write error, not a crash
  std::string mode = argc > 1 ? argv[1] : "";
  try {
    if (mode == "--version") {
      write_line({{"tool_version", proj::kToolVersion},
                  {"protocol_version", proj::kProtocolVersion},
                  {"index_schema_version", proj::kIndexSchemaVersion}});
      return 0;
    }
    if (mode == "--dump-ast") return argc == 4 ? dump_ast(argv[2], argv[3]) : usage();
    if (mode == "--help" || mode == "-h") {
      usage();
      return 0;
    }
    proj::protocol::Engine engine;
    if (mode == "--serve") {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line.size() > kMaxRequestBytes) {
          write_line(proj::protocol::make_response(nullptr, "", proj::protocol::status::kInvalidArguments, nullptr,
                                                   nullptr, {{"code", "request_too_large"}, {"message", "request exceeds 4 MiB"}}));
          continue;
        }
        write_line(engine.handle_text(line));
        if (std::ferror(stdout)) return 0;  // client went away
      }
      return 0;
    }
    if (!mode.empty()) return usage();
    std::string input;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
      input.append(buf, n);
      if (input.size() > kMaxRequestBytes) {
        write_line(proj::protocol::make_response(nullptr, "", proj::protocol::status::kInvalidArguments, nullptr,
                                                 nullptr, {{"code", "request_too_large"}, {"message", "request exceeds 4 MiB"}}));
        return 0;
      }
    }
    write_line(engine.handle_text(input));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "proj-engine: fatal: %s\n", e.what());
    write_line(proj::protocol::make_response(nullptr, "", proj::protocol::status::kInternalError, nullptr, nullptr,
                                             {{"code", "fatal"}, {"message", e.what()}}));
    return 70;
  }
}

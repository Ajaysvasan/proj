#include "scan/scanner.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/log.hpp"
#include "core/paths.hpp"

namespace proj {
namespace fs = std::filesystem;
namespace {

constexpr uintmax_t kMaxIgnoreFileBytes = 256 * 1024;

std::string join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

}  // namespace

const std::vector<std::string>& Scanner::default_excluded_dirs() {
  static const std::vector<std::string> dirs = {
      ".git", ".hg", ".svn", "node_modules", "bower_components", "jspm_packages", "vendor", "dist", "build",
      "out", "target", ".venv", "venv", "__pycache__", ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox",
      ".nox", ".eggs", "site-packages", ".gradle", ".idea", ".vscode", ".next", ".nuxt", ".svelte-kit",
      ".parcel-cache", ".turbo", ".cache", "coverage", ".nyc_output", ".terraform", "Pods", "DerivedData",
      "cmake-build-*", "*.egg-info", ".angular", ".expo", ".dart_tool"};
  return dirs;
}

const std::vector<std::string>& Scanner::default_excluded_files() {
  static const std::vector<std::string> files = {"*.min.js", "*.min.mjs", "*.bundle.js", "*.chunk.js",
                                                 "*.map", "*.pyc", "*.class", "*.o", "*.obj", "*.so",
                                                 "*.dylib", "*.a"};
  return files;
}

Scanner::Scanner(fs::path root, ScanOptions options, DiagnosticSink& diagnostics)
    : root_(std::move(root)), options_(std::move(options)), diagnostics_(diagnostics) {
  if (options_.default_excludes) {
    for (const auto& d : default_excluded_dirs()) {
      if (std::find(options_.include_defaults.begin(), options_.include_defaults.end(), d) !=
          options_.include_defaults.end()) {
        continue;
      }
      rules_.add_pattern(d + "/", "", "default");
    }
    for (const auto& f : default_excluded_files()) rules_.add_pattern(f, "", "default");
  }
  if (options_.use_gitignore) {
    load_enclosing_repository_rules();
    load_ignore_file(root_ / ".git" / "info" / "exclude", "", ".git/info/exclude");
  }
  load_ignore_file(root_ / ".projignore", "", ".projignore");
  for (const auto& p : options_.extra_excludes) rules_.add_pattern(p, "", "--exclude");
}

// When the project root is a subdirectory of a git work tree, git applies the
// .gitignore files of the directories above it and .git/info/exclude. Load
// them (outermost first, so deeper files take precedence) with a prefix that
// maps project-relative paths to paths relative to each file's directory.
void Scanner::load_enclosing_repository_rules() {
  std::vector<fs::path> ancestors;  // root's parent upward
  std::error_code ec;
  fs::path git_root;
  for (fs::path dir = root_.parent_path(); !dir.empty(); dir = dir.parent_path()) {
    ancestors.push_back(dir);
    if (fs::exists(dir / ".git", ec)) {
      git_root = dir;
      break;
    }
    if (dir == dir.parent_path()) break;
  }
  if (git_root.empty() || fs::exists(root_ / ".git", ec)) return;  // not inside a repo, or root is its own repo
  auto prefix_for = [&](const fs::path& dir) {
    auto rel = paths::relative_to_root(dir, root_);
    return rel.value_or("");
  };
  load_ignore_file(git_root / ".git" / "info" / "exclude", "", "<repo>/.git/info/exclude", prefix_for(git_root));
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    auto rel_from_git = paths::relative_to_root(git_root, *it).value_or("");
    std::string label = "<repo>/" + (rel_from_git.empty() ? std::string() : rel_from_git + "/") + ".gitignore";
    load_ignore_file(*it / ".gitignore", "", label, prefix_for(*it));
  }
}

void Scanner::load_ignore_file(const fs::path& file, const std::string& base, const std::string& source,
                               const std::string& prefix) {
  std::error_code ec;
  auto st = fs::symlink_status(file, ec);
  if (ec || !fs::is_regular_file(st)) return;
  auto size = fs::file_size(file, ec);
  if (ec) return;
  std::string label = base.empty() ? source : base + "/" + source;
  if (size > kMaxIgnoreFileBytes) {
    diagnostics_.add({"warn", "config", "ignore_file_too_large", label, 0,
                      "ignore file larger than 256 KiB was not applied"});
    return;
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    diagnostics_.add({"warn", "config", "ignore_file_unreadable", label, 0, "ignore file could not be read"});
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  rules_.add_text(ss.str(), base, label, prefix);
}

void Scanner::scan(const std::function<void(const ScanEntry&)>& visit) { walk_dir(root_, "", 0, visit); }

void Scanner::walk_dir(const fs::path& abs, const std::string& rel, int depth,
                       const std::function<void(const ScanEntry&)>& visit) {
  if (stats_.file_limit_reached) return;
  if (depth > options_.max_depth) {
    if (!stats_.depth_limit_reached) {
      diagnostics_.add({"warn", "resource", "directory_depth_limit", rel, 0,
                        "directory nesting exceeds " + std::to_string(options_.max_depth) +
                            " levels; deeper directories were not scanned"});
    }
    stats_.depth_limit_reached = true;
    return;
  }

  size_t rules_before = rules_.size();
  if (options_.use_gitignore) load_ignore_file(abs / ".gitignore", rel, ".gitignore");

  struct Child {
    std::string name;
    fs::file_status status;
  };
  std::vector<Child> children;
  std::error_code ec;
  fs::directory_iterator it(abs, fs::directory_options::none, ec);
  if (ec) {
    ++stats_.dirs_unreadable;
    diagnostics_.add({"warn", "source", "directory_unreadable", rel, 0, "cannot read directory: " + ec.message()});
    rules_.truncate(rules_before);
    return;
  }
  for (; it != fs::directory_iterator(); it.increment(ec)) {
    if (ec) {
      diagnostics_.add({"warn", "source", "directory_listing_error", rel, 0,
                        "directory listing stopped early: " + ec.message()});
      break;
    }
    std::error_code sec;
    auto status = it->symlink_status(sec);
    if (sec) continue;
    children.push_back({it->path().filename().string(), status});
  }
  std::sort(children.begin(), children.end(), [](const Child& a, const Child& b) { return a.name < b.name; });

  std::vector<std::string> subdirs;
  for (const auto& child : children) {
    std::string child_rel = join(rel, child.name);
    fs::path child_abs = abs / child.name;
    if (fs::is_symlink(child.status)) {
      // Symbolic links are never followed: they could leave the project root
      // or create cycles. Linked files are recorded so they stay visible.
      if (rules_.match(child_rel, false)) continue;
      std::error_code tec;
      if (fs::is_directory(fs::status(child_abs, tec))) {
        ++stats_.dirs_symlink;
        diagnostics_.add({"info", "source", "symlink_not_followed", child_rel, 0,
                          "symbolic link to a directory was not followed"});
        continue;
      }
      ScanEntry e;
      e.rel_path = child_rel;
      e.abs_path = child_abs;
      e.language = language_for_path(child_rel);
      e.status = file_status::kSymlink;
      e.note = "symbolic link not followed";
      ++stats_.files_seen;
      visit(e);
      continue;
    }
    if (fs::is_directory(child.status)) {
      if (const IgnoreRule* r = rules_.match(child_rel, true)) {
        ++stats_.dirs_excluded;
        diagnostics_.add({"debug", "config", "directory_excluded", child_rel, 0,
                          "excluded by " + r->source + " rule '" + r->pattern + "'"});
        continue;
      }
      subdirs.push_back(child.name);
      continue;
    }
    if (!fs::is_regular_file(child.status)) {
      ++stats_.special_files;
      continue;
    }
    if (stats_.files_seen >= options_.max_files) {
      stats_.file_limit_reached = true;
      diagnostics_.add({"warn", "resource", "file_limit_reached", rel, 0,
                        "file limit of " + std::to_string(options_.max_files) +
                            " reached; remaining files were not scanned"});
      break;
    }
    ++stats_.files_seen;
    ScanEntry e;
    e.rel_path = child_rel;
    e.abs_path = child_abs;
    e.language = language_for_path(child_rel);
    struct stat sb {};
    if (::lstat(child_abs.c_str(), &sb) == 0) {
      e.size = static_cast<uint64_t>(sb.st_size);
      e.mtime_s = static_cast<int64_t>(sb.st_mtime);
    }
    if (const IgnoreRule* r = rules_.match(child_rel, false)) {
      e.status = file_status::kExcluded;
      e.note = "excluded by " + r->source + " rule '" + r->pattern + "'";
    } else if (e.language == Language::None) {
      e.status = file_status::kUnsupported;
    } else if (e.size > options_.max_file_size) {
      e.status = file_status::kTooLarge;
      e.note = "larger than the " + std::to_string(options_.max_file_size) + " byte limit";
      // Large XML/YAML files are usually data, not configuration: informational only.
      diagnostics_.add({is_config_language(e.language) ? "info" : "warn", "resource", "file_too_large", child_rel, 0,
                        "skipped: " + std::to_string(e.size) + " bytes exceeds the " +
                            std::to_string(options_.max_file_size) + " byte limit"});
    } else {
      e.status = file_status::kPending;
    }
    visit(e);
  }

  for (const auto& name : subdirs) {
    if (stats_.file_limit_reached) break;
    walk_dir(abs / name, join(rel, name), depth + 1, visit);
  }
  rules_.truncate(rules_before);
}

}  // namespace proj

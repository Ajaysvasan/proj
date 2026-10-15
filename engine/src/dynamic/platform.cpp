#include "dynamic/platform.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <unistd.h>
#endif

extern char** environ;

namespace proj::dynamic::platform {
namespace fs = std::filesystem;

bool process_alive(int64_t pid) {
  if (pid <= 0) return false;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool owned_by_me(int64_t pid) {
#if defined(__APPLE__)
  struct kinfo_proc info {};
  size_t size = sizeof(info);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return false;
  return info.kp_eproc.e_ucred.cr_uid == ::getuid();
#else
  struct stat st {};
  std::string p = "/proc/" + std::to_string(pid);
  return ::stat(p.c_str(), &st) == 0 && st.st_uid == ::getuid();
#endif
}

std::optional<std::string> command_line(int64_t pid) {
#if defined(__APPLE__)
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
  size_t size = 0;
  if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size == 0) return std::nullopt;
  std::string buf(size, '\0');
  if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return std::nullopt;
  int argc = 0;
  std::memcpy(&argc, buf.data(), sizeof(int));
  size_t i = sizeof(int);
  while (i < size && buf[i] != '\0') ++i;  // executable path
  while (i < size && buf[i] == '\0') ++i;
  std::string out;
  for (int a = 0; a < argc && i < size; ++a) {
    size_t end = buf.find('\0', i);
    if (end == std::string::npos) end = size;
    if (!out.empty()) out += ' ';
    out.append(buf, i, end - i);
    i = end + 1;
  }
  return out;
#else
  std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string s = ss.str();
  while (!s.empty() && s.back() == '\0') s.pop_back();
  for (char& c : s) {
    if (c == '\0') c = ' ';
  }
  return s;
#endif
}

namespace {
fs::path temp_dir() {
#if defined(__APPLE__)
  char buf[1024];
  size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf));
  if (n > 0 && n <= sizeof(buf)) return fs::path(buf);
#endif
  return fs::path("/tmp");
}

std::string user_name() {
  struct passwd* pw = ::getpwuid(::getuid());
  return pw && pw->pw_name ? pw->pw_name : "";
}
}  // namespace

std::vector<int64_t> user_jvms() {
  std::vector<int64_t> out;
  std::string user = user_name();
  if (user.empty()) return out;
  fs::path dir = temp_dir() / ("hsperfdata_" + user);
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent* e = ::readdir(d)) {
    char* end = nullptr;
    long long pid = std::strtoll(e->d_name, &end, 10);
    if (end && *end == '\0' && pid > 0 && process_alive(pid)) out.push_back(pid);
    if (out.size() >= 4096) break;
  }
  ::closedir(d);
  return out;
}

std::optional<Child> spawn_capture(const std::vector<std::string>& argv, std::string* error) {
  int fds[2];
  if (::pipe(fds) != 0) {
    if (error) *error = std::string("pipe: ") + std::strerror(errno);
    return std::nullopt;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = -1;
  int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    if (error) *error = "cannot start " + argv[0] + ": " + std::strerror(rc);
    return std::nullopt;
  }
  ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  return Child{static_cast<int>(pid), fds[0]};
}

std::string git_revision(const fs::path& root) {
  auto read_line = [](const fs::path& p) -> std::string {
    std::ifstream in(p);
    std::string line;
    if (in) std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
  };
  // The project may be a subdirectory of a checkout.
  for (fs::path dir = root; !dir.empty(); dir = dir.parent_path()) {
    fs::path git = dir / ".git";
    std::error_code ec;
    if (fs::is_directory(git, ec)) {
      std::string head = read_line(git / "HEAD");
      if (head.rfind("ref: ", 0) != 0) return head;  // detached
      std::string ref = head.substr(5);
      std::string sha = read_line(git / ref);
      if (!sha.empty()) return sha;
      std::ifstream packed(git / "packed-refs");
      std::string line;
      while (std::getline(packed, line)) {
        auto sp = line.find(' ');
        if (sp != std::string::npos && line.substr(sp + 1) == ref) return line.substr(0, sp);
      }
      return "";
    }
    if (dir == dir.parent_path()) break;
  }
  return "";
}

}  // namespace proj::dynamic::platform

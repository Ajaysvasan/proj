#include "dynamic/service.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>

#include "core/json.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "core/version.hpp"
#include "db/sqlite.hpp"
#include "dynamic/platform.hpp"
#include "dynamic/runtime_store.hpp"
#include "index/registry.hpp"

namespace proj::dynamic {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

fs::path service_dir(const fs::path& data_dir) { return data_dir / "dynamic"; }
fs::path service_file(const fs::path& data_dir) { return service_dir(data_dir) / "service.json"; }
fs::path runtime_store_path(const fs::path& data_dir, int64_t project_id) {
  return data_dir / "runtime" / ("project-" + std::to_string(project_id) + ".db");
}

std::vector<std::string> java_packages(const fs::path& index_path) {
  db::Database db(index_path, db::OpenMode::ReadOnly);
  auto st = db.prepare(
      "SELECT DISTINCT IFNULL(module_name, '') FROM files WHERE language = 'java' AND status IN ('parsed','syntax_errors')");
  std::vector<std::string> out;
  while (st.step()) out.push_back(st.text(0));
  return out;
}

namespace {
std::vector<std::string> segments(const std::string& pkg) {
  std::vector<std::string> out;
  std::stringstream ss(pkg);
  std::string s;
  while (std::getline(ss, s, '.')) out.push_back(s);
  return out;
}

std::string join_segments(const std::vector<std::string>& s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n && i < s.size(); ++i) out += (i ? "." : "") + s[i];
  return out;
}
}  // namespace

std::vector<std::string> include_roots(std::vector<std::string> packages) {
  std::sort(packages.begin(), packages.end());
  packages.erase(std::unique(packages.begin(), packages.end()), packages.end());
  std::map<std::string, std::vector<std::vector<std::string>>> groups;
  bool default_package = false;
  for (const auto& p : packages) {
    if (p.empty()) {
      default_package = true;
      continue;
    }
    auto seg = segments(p);
    groups[join_segments(seg, 2)].push_back(seg);
  }
  std::vector<std::string> roots;
  for (const auto& [key, members] : groups) {
    size_t common = members[0].size();
    for (const auto& m : members) {
      size_t i = 0;
      while (i < common && i < m.size() && m[i] == members[0][i]) ++i;
      common = i;
    }
    roots.push_back(join_segments(members[0], std::max<size_t>(common, 1)));
  }
  std::sort(roots.begin(), roots.end());
  std::vector<std::string> out;
  for (const auto& r : roots) {
    bool covered = std::any_of(out.begin(), out.end(), [&](const std::string& o) { return r == o || r.rfind(o + ".", 0) == 0; });
    if (!covered) out.push_back(r);
  }
  if (default_package) out.push_back("<default>");
  return out;
}

namespace {

int g_signal_pipe[2] = {-1, -1};

void on_signal(int) {
  char c = 1;
  ssize_t ignored = ::write(g_signal_pipe[1], &c, 1);
  (void)ignored;
}

void set_nonblock(int fd) {
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string random_token() {
  std::random_device rd;
  std::string out;
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) out += hex[rd() & 15];
  return out;
}

void write_private(const fs::path& path, const std::string& text) {
  fs::path tmp = path;
  tmp += ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot write " + tmp.string() + ": " + std::strerror(errno));
  size_t done = 0;
  while (done < text.size()) {
    ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n <= 0) {
      ::close(fd);
      throw std::runtime_error("cannot write " + tmp.string());
    }
    done += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  fs::rename(tmp, path);
}

json read_json_file(const fs::path& path, json fallback) {
  std::ifstream in(path);
  if (!in) return fallback;
  std::stringstream ss;
  ss << in.rdbuf();
  json j = json::parse(ss.str(), nullptr, false);
  return j.is_discarded() ? fallback : j;
}

constexpr size_t kMaxLine = 32u << 20;  // a trace with the default limits is well below 2 MiB
constexpr int kBatchMs = 200;
constexpr int kAttachTimeoutMs = 30000;
constexpr int kHelloTimeoutMs = 15000;
constexpr int kStopTimeoutMs = 5000;
constexpr int kMaxAttachAttempts = 4;
constexpr size_t kHistory = 32;

struct Conn {
  enum class Role { Unknown, Agent, Control };
  int fd = -1;
  Role role = Role::Unknown;
  std::string in;
  std::string out;
  bool closed = false;
  bool close_after_write = false;
  // agent connections
  std::string project;
  int64_t pid = -1;
  int64_t session = -1;  // runtime store session; -1 while on standby (project not monitored)
  json hello;
  std::string bye;
  bool captured = false;
};

struct Attach {
  platform::Child child;
  std::string project;
  int64_t target = -1;
  std::string output;
  Clock::time_point deadline;
};

struct Monitor {
  std::string project;
  ProjectRecord record;
  std::vector<std::string> include;
  std::string agent_args;
  std::unique_ptr<RuntimeStore> store;
  Clock::time_point batch_started;
  std::string state = "STOPPED";
  std::string since;
  std::deque<json> history;
  std::set<int> agents;
  int64_t pid = -1;     // attach to this process
  std::string match;    // attach to JVMs whose command line contains this
  int64_t attaching = -1;
  bool awaiting_hello = false;
  Clock::time_point hello_deadline;
  int attempts = 0;
  Clock::time_point next_attempt;
  std::set<int64_t> given_up;
  std::string last_error;
  bool stopping = false;
  Clock::time_point stop_deadline;
};

class Service {
 public:
  explicit Service(ServiceOptions o) : opt_(std::move(o)), registry_(opt_.data_dir) {}
  int run();

 private:
  ServiceOptions opt_;
  Registry registry_;
  int listen_fd_ = -1;
  int lock_fd_ = -1;
  int port_ = 0;
  std::string token_;
  std::string started_at_;
  bool quit_ = false;
  bool had_command_ = false;
  Clock::time_point idle_exit_ = Clock::now() + std::chrono::seconds(15);
  std::map<int, Conn> conns_;
  std::map<std::string, Monitor> monitors_;
  std::vector<Attach> attaches_;
  std::vector<std::pair<int, std::set<std::string>>> stop_waiters_;  // control fd -> projects still stopping

  void setup();
  void cleanup();
  void loop_once();
  int timeout_ms();
  void accept_all();
  void read_conn(Conn& c);
  void write_conn(Conn& c);
  void handle_line(Conn& c, const std::string& line);
  void send(Conn& c, const json& j);
  void close_conn(Conn& c, const std::string& why);
  void on_agent_closed(Conn& c);

  // control
  void control(Conn& c, const json& req);
  json status_json(const std::string& project);
  json monitor_json(const Monitor& m);
  json start(const json& req, json& results);
  bool start_one(const std::string& project, int64_t pid, const std::string& match, const json& include, json& result);
  void stop(Conn& c, const std::string& project);
  void finish_stop(Monitor& m);
  void answer_stop_waiters();

  // agents
  void agent_hello(Conn& c, const json& hello);
  void agent_message(Conn& c, const json& msg);
  RuntimeStore& store(Monitor& m);
  void commit(Monitor& m);

  // attach
  bool connected(const Monitor& m, int64_t pid);
  bool needs_attach(const Monitor& m);
  void maybe_attach(Monitor& m);
  void start_attach(Monitor& m, int64_t pid, const std::string& how);
  void attach_output(Attach& a);
  void finish_attach(Attach& a);
  void attach_failed(Monitor& m, int64_t target, const std::string& error);
  void discover();

  void set_state(Monitor& m, const std::string& state, const std::string& detail);
  void save_monitors();
  json config();
  void save_config(const json& cfg);
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void Service::setup() {
  fs::path dir = service_dir(opt_.data_dir);
  fs::create_directories(dir);
  fs::create_directories(opt_.data_dir / "runtime");
  fs::create_directories(opt_.data_dir / "logs");
  ::chmod(dir.c_str(), 0700);
  fs::path lock = dir / "service.lock";
  lock_fd_ = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    throw std::runtime_error("another proj dynamic service is already running for " + opt_.data_dir.string());
  }
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 32) != 0) {
    throw std::runtime_error(std::string("cannot listen on 127.0.0.1: ") + std::strerror(errno));
  }
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  set_nonblock(listen_fd_);
  token_ = random_token();
  started_at_ = iso_now();
  if (::pipe(g_signal_pipe) == 0) {
    set_nonblock(g_signal_pipe[0]);
    set_nonblock(g_signal_pipe[1]);
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
  }
  write_private(service_file(opt_.data_dir), json{{"pid", ::getpid()},
                                                  {"port", port_},
                                                  {"token", token_},
                                                  {"version", kToolVersion},
                                                  {"started_at", started_at_},
                                                  {"agent_jar", opt_.agent_jar.string()}}
                                                 .dump());
  log::info("dynamic", "service listening on 127.0.0.1:" + std::to_string(port_));
  // Monitoring that was active when the service last ran resumes (sessions it left open were
  // marked interrupted by the store).
  json saved = read_json_file(service_dir(opt_.data_dir) / "monitors.json", json::object());
  if (saved.contains("monitors") && saved["monitors"].is_array()) {
    for (const auto& m : saved["monitors"]) {
      if (!m.is_object() || !m.contains("project")) continue;
      json result;
      start_one(m["project"].get<std::string>(), m.value("pid", int64_t{-1}), m.value("match", std::string()),
                m.value("include", json()), result);
    }
  }
}

void Service::cleanup() {
  for (auto& [name, m] : monitors_) commit(m);
  for (auto& a : attaches_) {
    ::kill(a.child.pid, SIGKILL);
    ::waitpid(a.child.pid, nullptr, 0);
    ::close(a.child.out_fd);
  }
  for (auto& [fd, c] : conns_) ::close(fd);
  conns_.clear();
  std::error_code ec;
  fs::remove(service_file(opt_.data_dir), ec);
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (lock_fd_ >= 0) ::close(lock_fd_);
  log::info("dynamic", "service stopped");
}

int Service::run() {
  setup();
  while (!quit_) loop_once();
  cleanup();
  return 0;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

int Service::timeout_ms() {
  auto now = Clock::now();
  Clock::time_point next = Clock::time_point::max();
  auto consider = [&](Clock::time_point t) { next = std::min(next, t); };
  for (auto& [name, m] : monitors_) {
    if (m.store && m.store->in_batch()) consider(m.batch_started + std::chrono::milliseconds(kBatchMs));
    if (m.awaiting_hello) consider(m.hello_deadline);
    if (m.stopping) consider(m.stop_deadline);
    // Wake up only for an attach still to do: a match rule's next look, or a pid retry.
    if (!m.stopping && m.attaching < 0 && !m.awaiting_hello && (!m.match.empty() || needs_attach(m))) {
      consider(m.next_attempt);
    }
  }
  for (auto& a : attaches_) consider(a.deadline);
  if (!had_command_ && monitors_.empty()) consider(idle_exit_);
  if (next == Clock::time_point::max()) return -1;  // idle: wait for a connection, no wakeups
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
  return static_cast<int>(std::clamp<long long>(ms, 0, 60000));
}

void Service::loop_once() {
  std::vector<pollfd> fds;
  fds.push_back({listen_fd_, POLLIN, 0});
  fds.push_back({g_signal_pipe[0], POLLIN, 0});
  std::vector<int> conn_fds;
  for (auto& [fd, c] : conns_) {
    short ev = POLLIN;
    if (!c.out.empty()) ev |= POLLOUT;
    fds.push_back({fd, ev, 0});
    conn_fds.push_back(fd);
  }
  size_t attach_base = fds.size();
  for (auto& a : attaches_) fds.push_back({a.child.out_fd, POLLIN, 0});
  int n = ::poll(fds.data(), fds.size(), timeout_ms());
  if (n < 0 && errno != EINTR) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
  if (n > 0) {
    if (fds[1].revents & POLLIN) {
      char buf[16];
      while (::read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {
      }
      log::info("dynamic", "signal received: stopping all monitoring");
      Conn none;
      stop(none, "");
    }
    if (fds[0].revents & POLLIN) accept_all();
    for (size_t i = 0; i < conn_fds.size(); ++i) {
      const pollfd& p = fds[2 + i];
      auto it = conns_.find(conn_fds[i]);
      if (it == conns_.end() || !p.revents) continue;
      if (p.revents & (POLLIN | POLLHUP | POLLERR)) read_conn(it->second);
      if (!it->second.closed && (p.revents & POLLOUT)) write_conn(it->second);
    }
    for (size_t i = 0; i < attaches_.size(); ++i) {
      if (fds[attach_base + i].revents) attach_output(attaches_[i]);
    }
  }
  // Attach helpers that finished (their output pipe closed).
  for (size_t i = 0; i < attaches_.size();) {
    if (attaches_[i].child.out_fd < 0 || Clock::now() > attaches_[i].deadline) {
      Attach a = std::move(attaches_[i]);
      attaches_.erase(attaches_.begin() + static_cast<long>(i));
      finish_attach(a);
    } else {
      ++i;
    }
  }
  auto now = Clock::now();
  for (auto& [name, m] : monitors_) {
    if (m.store && m.store->in_batch() && now - m.batch_started >= std::chrono::milliseconds(kBatchMs)) commit(m);
    if (m.awaiting_hello && now > m.hello_deadline) {
      m.awaiting_hello = false;
      attach_failed(m, m.attaching, "the agent was loaded but did not connect within " + std::to_string(kHelloTimeoutMs / 1000) + " s");
    }
    if (m.stopping && now > m.stop_deadline) {
      for (int fd : std::set<int>(m.agents)) {
        auto it = conns_.find(fd);
        if (it != conns_.end()) close_conn(it->second, "the agent did not confirm the stop in time");
      }
    }
  }
  discover();
  for (auto& [name, m] : monitors_) maybe_attach(m);
  // Remove closed connections (their monitors are updated in close_conn).
  for (auto it = conns_.begin(); it != conns_.end();) {
    if (it->second.closed) {
      ::close(it->first);
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
  // Monitors whose stop completed.
  for (auto it = monitors_.begin(); it != monitors_.end();) {
    if (it->second.stopping && it->second.agents.empty()) {
      finish_stop(it->second);
      it = monitors_.erase(it);
      save_monitors();
    } else {
      ++it;
    }
  }
  answer_stop_waiters();
  bool waiting = !stop_waiters_.empty() ||
                 std::any_of(conns_.begin(), conns_.end(), [](const auto& kv) { return !kv.second.out.empty(); });
  if (monitors_.empty() && !waiting && (had_command_ || Clock::now() > idle_exit_)) quit_ = true;
}

void Service::accept_all() {
  while (true) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) return;
    set_nonblock(fd);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    Conn c;
    c.fd = fd;
    conns_[fd] = std::move(c);
  }
}

void Service::read_conn(Conn& c) {
  char buf[65536];
  while (!c.closed) {
    ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      c.in.append(buf, static_cast<size_t>(n));
      size_t start = 0;
      while (!c.closed) {
        size_t nl = c.in.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = c.in.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty()) handle_line(c, line);
      }
      c.in.erase(0, start);
      if (c.in.size() > kMaxLine) close_conn(c, "line too long");
      continue;
    }
    if (n == 0) {
      close_conn(c, c.bye.empty() ? "connection lost" : c.bye);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    if (errno == EINTR) continue;
    close_conn(c, std::string("read error: ") + std::strerror(errno));
    return;
  }
}

void Service::write_conn(Conn& c) {
  while (!c.out.empty()) {
    ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), 0);
    if (n > 0) {
      c.out.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n < 0 && errno == EINTR) continue;
    close_conn(c, "write failed");
    return;
  }
  if (c.close_after_write) close_conn(c, "done");
}

void Service::send(Conn& c, const json& j) {
  if (c.fd < 0 || c.closed) return;
  c.out += j.dump(-1, ' ', false, json::error_handler_t::replace);
  c.out += '\n';
  write_conn(c);
}

void Service::close_conn(Conn& c, const std::string& why) {
  if (c.closed) return;
  c.closed = true;
  if (c.role == Conn::Role::Agent) {
    c.bye = c.bye.empty() ? why : c.bye;
    on_agent_closed(c);
  }
}

void Service::handle_line(Conn& c, const std::string& line) {
  json msg = json::parse(line, nullptr, false);
  if (msg.is_discarded() || !msg.is_object()) {
    close_conn(c, "malformed message");
    return;
  }
  if (c.role == Conn::Role::Unknown) {
    if (msg.value("token", std::string()) != token_) {
      send(c, {{"type", "welcome"}, {"accept", false}, {"reason", "invalid token"}});
      c.close_after_write = true;
      write_conn(c);
      return;
    }
    if (msg.value("type", std::string()) == "hello") {
      c.role = Conn::Role::Agent;
      agent_hello(c, msg);
    } else {
      c.role = Conn::Role::Control;
      control(c, msg);
    }
    return;
  }
  if (c.role == Conn::Role::Agent) {
    agent_message(c, msg);
  } else {
    control(c, msg);
  }
}

// ---------------------------------------------------------------------------
// Agents
// ---------------------------------------------------------------------------

RuntimeStore& Service::store(Monitor& m) {
  if (!m.store) {
    m.store = std::make_unique<RuntimeStore>(runtime_store_path(opt_.data_dir, m.record.id));
    // Only this service writes the store: sessions still "capturing" belong to a previous run.
    int64_t n = m.store->mark_interrupted("the dynamic service stopped while the session was open");
    if (n > 0) log::info("dynamic", m.project + ": " + std::to_string(n) + " session(s) of a previous service marked interrupted");
  }
  if (!m.store->in_batch()) {
    m.store->begin();
    m.batch_started = Clock::now();
  }
  return *m.store;
}

void Service::commit(Monitor& m) {
  if (!m.store || !m.store->in_batch()) return;
  try {
    m.store->commit();
    m.store->retain();
  } catch (const std::exception& e) {
    log::error("dynamic", m.project + ": writing runtime observations failed: " + e.what());
  }
}

void Service::agent_hello(Conn& c, const json& hello) {
  c.project = hello.value("project", std::string());
  c.pid = hello.value("pid", int64_t{-1});
  c.hello = hello;
  if (!registry_.find_by_name(c.project)) {
    send(c, {{"type", "welcome"}, {"accept", false}, {"reason", "project '" + c.project + "' is not registered with proj"}});
    c.close_after_write = true;
    write_conn(c);
    return;
  }
  auto it = monitors_.find(c.project);
  bool monitored = it != monitors_.end() && !it->second.stopping;
  send(c, {{"type", "welcome"}, {"accept", true}, {"enabled", monitored}});
  if (!monitored) {
    log::info("dynamic", c.project + ": agent of pid " + std::to_string(c.pid) + " on standby (not monitored)");
    return;  // standby: enabled when `proj start dynamic` names this project
  }
  Monitor& m = it->second;
  std::string mode = hello.value("mode", std::string());
  if (c.pid == m.attaching) {
    m.awaiting_hello = false;
    m.attaching = -1;
    m.attempts = 0;
  } else {
    set_state(m, "TARGET_DISCOVERED", "pid " + std::to_string(c.pid) + " connected (agent loaded at " +
                                          (mode == "attach" ? "attach" : "JVM startup") + ")");
  }
  json context = {{"index_identity", m.record.last_success_at.value_or("")},
                  {"source_revision", platform::git_revision(m.record.root_path)}};
  c.session = store(m).open_session(hello, context);
  m.agents.insert(c.fd);
  bool complete = mode != "attach" && hello.value("since_start", false);
  set_state(m, "ATTACHED", "pid " + std::to_string(c.pid) + ": " +
                               (complete ? "recording since JVM startup"
                                         : "attached to a running JVM: startup before the attach is not recorded"));
}

void Service::agent_message(Conn& c, const json& msg) {
  std::string type = msg.value("type", std::string());
  if (type == "bye") {
    c.bye = msg.value("reason", std::string("bye"));
    return;
  }
  if (c.session < 0) return;  // standby agents record nothing
  auto it = monitors_.find(c.project);
  if (it == monitors_.end()) return;
  Monitor& m = it->second;
  try {
    RuntimeStore& s = store(m);
    if (type == "method") {
      s.method(c.session, msg);
    } else if (type == "trace") {
      s.trace(c.session, msg);
      if (!c.captured) {
        c.captured = true;
        if (m.state == "ATTACHED") set_state(m, "CAPTURING", "first trace from pid " + std::to_string(c.pid));
      }
    } else if (type == "uncaught") {
      s.uncaught(c.session, msg);
    } else if (type == "stats") {
      s.session_stats(c.session, msg);
    }
  } catch (const std::exception& e) {
    log::warn("dynamic", m.project + ": dropped a " + type + " message: " + e.what());
  }
}

void Service::on_agent_closed(Conn& c) {
  auto it = monitors_.find(c.project);
  if (it == monitors_.end() || c.session < 0) return;
  Monitor& m = it->second;
  m.agents.erase(c.fd);
  set_state(m, "FLUSHING", "pid " + std::to_string(c.pid) + ": " + c.bye);
  std::string status = c.bye == "jvm_exit" ? "ended" : (c.bye == "stopped" || m.stopping ? "stopped" : "interrupted");
  try {
    RuntimeStore& s = store(m);
    s.close_session(c.session, status, c.bye == "jvm_exit" ? "the JVM exited" : c.bye);
  } catch (const std::exception& e) {
    log::error("dynamic", m.project + ": closing session failed: " + e.what());
  }
  commit(m);
  if (m.stopping) return;
  if (m.agents.empty()) {
    set_state(m, "IDLE", m.pid > 0 ? "target exited" : "waiting for the next JVM");
    if (m.pid > 0 && !platform::process_alive(m.pid)) m.pid = -1;  // a pid does not come back
  } else {
    set_state(m, "CAPTURING", std::to_string(m.agents.size()) + " JVM(s) still connected");
  }
}

// ---------------------------------------------------------------------------
// Attach to running JVMs
// ---------------------------------------------------------------------------

bool Service::connected(const Monitor& m, int64_t pid) {
  return std::any_of(m.agents.begin(), m.agents.end(), [&](int fd) {
    auto it = conns_.find(fd);
    return it != conns_.end() && it->second.pid == pid;
  });
}

bool Service::needs_attach(const Monitor& m) { return m.pid > 0 && !m.given_up.count(m.pid) && !connected(m, m.pid); }

void Service::maybe_attach(Monitor& m) {
  if (m.stopping || m.attaching >= 0 || m.awaiting_hello || !needs_attach(m)) return;
  if (Clock::now() < m.next_attempt) return;
  if (!platform::process_alive(m.pid)) {
    set_state(m, "IDLE", "pid " + std::to_string(m.pid) + " is not running");
    m.pid = -1;
    return;
  }
  start_attach(m, m.pid, "pid given to `proj start dynamic`");
}

void Service::discover() {
  bool any = std::any_of(monitors_.begin(), monitors_.end(), [&](const auto& kv) {
    const Monitor& m = kv.second;
    return !m.match.empty() && !m.stopping && m.attaching < 0 && !m.awaiting_hello && Clock::now() >= m.next_attempt;
  });
  if (!any) return;
  std::vector<int64_t> jvms = platform::user_jvms();
  std::set<int64_t> connected;
  for (auto& [fd, c] : conns_) {
    if (c.role == Conn::Role::Agent) connected.insert(c.pid);
  }
  for (auto& [name, m] : monitors_) {
    if (m.match.empty() || m.stopping || m.attaching >= 0 || m.awaiting_hello || Clock::now() < m.next_attempt) continue;
    m.next_attempt = Clock::now() + std::chrono::milliseconds(opt_.discovery_ms);
    for (int64_t pid : jvms) {
      if (pid == ::getpid() || connected.count(pid) || m.given_up.count(pid) || !platform::owned_by_me(pid)) continue;
      auto cmd = platform::command_line(pid);
      // Never our own attach helpers, nor JVMs that already load the agent at startup.
      if (!cmd || cmd->find(m.match) == std::string::npos || cmd->find("proj-agent.jar") != std::string::npos) continue;
      start_attach(m, pid, "command line contains '" + m.match + "'");
      break;
    }
  }
}

void Service::start_attach(Monitor& m, int64_t pid, const std::string& how) {
  set_state(m, "TARGET_DISCOVERED", "pid " + std::to_string(pid) + " (" + how + ")");
  std::string error;
  auto child = platform::spawn_capture({opt_.java, "-jar", opt_.agent_jar.string(), "--attach", std::to_string(pid), m.agent_args}, &error);
  if (!child) {
    attach_failed(m, pid, error);
    return;
  }
  m.attaching = pid;
  Attach a;
  a.child = *child;
  a.project = m.project;
  a.target = pid;
  a.deadline = Clock::now() + std::chrono::milliseconds(kAttachTimeoutMs);
  attaches_.push_back(std::move(a));
  set_state(m, "ATTACHING", "loading the agent into pid " + std::to_string(pid));
}

void Service::attach_output(Attach& a) {
  char buf[4096];
  while (true) {
    ssize_t n = ::read(a.child.out_fd, buf, sizeof(buf));
    if (n > 0) {
      if (a.output.size() < 65536) a.output.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {
      ::close(a.child.out_fd);
      a.child.out_fd = -1;
    }
    return;  // EAGAIN: more later
  }
}

void Service::finish_attach(Attach& a) {
  if (a.child.out_fd >= 0) {  // timed out
    ::kill(a.child.pid, SIGKILL);
    ::close(a.child.out_fd);
  }
  int status = 0;
  ::waitpid(a.child.pid, &status, 0);
  auto it = monitors_.find(a.project);
  if (it == monitors_.end()) return;
  Monitor& m = it->second;
  m.attaching = -1;
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && a.output.find("\"attached\":true") != std::string::npos;
  if (!ok) {
    std::string detail = a.output;
    auto at = detail.find("\"error\":\"");
    if (at != std::string::npos) detail = detail.substr(at + 9, detail.find('"', at + 9) - at - 9);
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
    if (detail.size() > 400) detail = detail.substr(0, 400);
    attach_failed(m, a.target, "attach to pid " + std::to_string(a.target) + " failed: " + (detail.empty() ? "no output" : detail));
    return;
  }
  // Loaded: the agent connects and says hello (possibly already did).
  if (connected(m, a.target)) return;
  m.attaching = a.target;
  m.awaiting_hello = true;
  m.hello_deadline = Clock::now() + std::chrono::milliseconds(kHelloTimeoutMs);
}

void Service::attach_failed(Monitor& m, int64_t target, const std::string& error) {
  m.attaching = -1;
  m.awaiting_hello = false;
  m.last_error = error;
  m.attempts++;
  if (m.attempts >= kMaxAttachAttempts) {
    if (target > 0) m.given_up.insert(target);
    m.attempts = 0;
    set_state(m, "IDLE", error + "; gave up after " + std::to_string(kMaxAttachAttempts) + " attempts");
    if (m.pid == target) m.pid = -1;
    return;
  }
  int delay = std::min(60, 2 << m.attempts);  // 4, 8, 16 s
  m.next_attempt = Clock::now() + std::chrono::seconds(delay);
  set_state(m, "IDLE", error + "; retrying in " + std::to_string(delay) + " s");
}

// ---------------------------------------------------------------------------
// Control: start, stop, status
// ---------------------------------------------------------------------------

void Service::control(Conn& c, const json& req) {
  std::string op = req.value("op", std::string());
  // A status probe is not a command: a new service with nothing to monitor waits for its first
  // start (up to the idle grace period) instead of exiting after the probe.
  if (op == "start") had_command_ = true;
  json resp = {{"ok", true}};
  try {
    if (op == "status") {
      resp.update(status_json(req.value("project", std::string())));
    } else if (op == "start") {
      json results = json::array();
      json err = start(req, results);
      if (!err.is_null()) {
        resp["ok"] = false;
        resp["error"] = err;
      }
      resp["results"] = results;
      resp.update(status_json(""));
    } else if (op == "stop") {
      stop(c, req.value("project", std::string()));
      return;  // answered when the stop completes
    } else {
      resp = {{"ok", false}, {"error", {{"code", "unknown_op"}, {"message", "unknown operation '" + op + "'"}}}};
    }
  } catch (const std::exception& e) {
    resp = {{"ok", false}, {"error", {{"code", "internal_error"}, {"message", e.what()}}}};
  }
  send(c, resp);
}

json Service::monitor_json(const Monitor& m) {
  json sessions = json::array();
  for (int fd : m.agents) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) continue;
    const Conn& c = it->second;
    std::string mode = c.hello.value("mode", std::string());
    sessions.push_back({{"pid", c.pid},
                        {"session", c.session},
                        {"mode", mode},
                        {"startup_coverage", mode == "attach" ? "attached_late"
                                             : (c.hello.value("since_start", false) ? "complete" : "interrupted")},
                        {"jvm", c.hello.value("jvm", json::object())}});
  }
  std::string include;
  for (const auto& p : m.include) include += (include.empty() ? "" : ":") + p;
  json j = {{"project", m.project},
            {"state", m.state},
            {"since", m.since},
            {"include", include},
            {"agent_option", "-javaagent:" + opt_.agent_jar.string() + "=" + m.agent_args},
            {"sessions", sessions},
            {"history", json(m.history)},
            {"store", runtime_store_path(opt_.data_dir, m.record.id).string()}};
  if (m.pid > 0) j["pid"] = m.pid;
  if (!m.match.empty()) j["match"] = m.match;
  if (!m.last_error.empty()) j["last_error"] = m.last_error;
  return j;
}

json Service::status_json(const std::string& project) {
  json monitors = json::array();
  for (const auto& [name, m] : monitors_) {
    if (project.empty() || name == project) monitors.push_back(monitor_json(m));
  }
  return {{"service",
           {{"pid", ::getpid()}, {"port", port_}, {"started_at", started_at_}, {"version", kToolVersion},
            {"agent_jar", opt_.agent_jar.string()}, {"data_dir", opt_.data_dir.string()}}},
          {"monitors", monitors}};
}

json Service::config() { return read_json_file(service_dir(opt_.data_dir) / "config.json", json{{"projects", json::object()}}); }

void Service::save_config(const json& cfg) { write_private(service_dir(opt_.data_dir) / "config.json", cfg.dump(2)); }

void Service::save_monitors() {
  json list = json::array();
  for (const auto& [name, m] : monitors_) {
    if (m.stopping) continue;
    json e = {{"project", name}};
    if (m.pid > 0) e["pid"] = m.pid;
    if (!m.match.empty()) e["match"] = m.match;
    list.push_back(e);
  }
  write_private(service_dir(opt_.data_dir) / "monitors.json", json{{"monitors", list}}.dump(2));
}

json Service::start(const json& req, json& results) {
  std::string project = req.value("project", std::string());
  int64_t pid = req.contains("pid") && req["pid"].is_number_integer() ? req["pid"].get<int64_t>() : -1;
  std::string match = req.value("match", std::string());
  json include = req.contains("include") && req["include"].is_array() ? req["include"] : json();
  if (!project.empty()) {
    json result;
    bool ok = start_one(project, pid, match, include, result);
    results.push_back(result);
    save_monitors();
    return ok ? json() : result["error"];
  }
  // All configured projects (configured by an earlier `proj start dynamic <project>`).
  json cfg = config();
  if (!cfg.contains("projects") || cfg["projects"].empty()) {
    return {{"code", "no_configured_projects"},
            {"message", "no project is configured for dynamic monitoring yet; run `proj start dynamic <project>` first"}};
  }
  bool any = false;
  for (auto& [name, settings] : cfg["projects"].items()) {
    json result;
    any |= start_one(name, -1, settings.value("match", std::string()), settings.value("include", json()), result);
    results.push_back(result);
  }
  save_monitors();
  return any ? json() : json{{"code", "nothing_started"}, {"message", "no configured project could be monitored"}};
}

bool Service::start_one(const std::string& project, int64_t pid, const std::string& match, const json& include_override,
                        json& result) {
  result = {{"project", project}};
  auto fail = [&](const std::string& code, const std::string& message) {
    result["ok"] = false;
    result["error"] = {{"code", code}, {"message", message}};
    log::warn("dynamic", project + ": not monitored: " + message);
    return false;
  };
  auto rec = registry_.find_by_name(project);
  if (!rec) return fail("unknown_project", "no project named '" + project + "' is registered");
  std::error_code ec;
  if (!fs::exists(rec->index_path, ec)) return fail("index_missing", "project '" + project + "' has no index; run `proj add` again");
  if (!fs::exists(opt_.agent_jar, ec)) {
    return fail("agent_missing", "the Java agent was not found at " + opt_.agent_jar.string() + " (build it with a JDK installed)");
  }
  if (pid > 0) {
    if (!platform::process_alive(pid)) return fail("target_not_found", "no process with pid " + std::to_string(pid));
    if (!platform::owned_by_me(pid)) return fail("target_not_owned", "pid " + std::to_string(pid) + " belongs to another user");
  }
  std::vector<std::string> include;
  if (include_override.is_array() && !include_override.empty()) {
    for (const auto& p : include_override) include.push_back(p.get<std::string>());
  } else {
    try {
      include = include_roots(java_packages(rec->index_path));
    } catch (const std::exception& e) {
      return fail("index_unreadable", std::string("cannot read the index: ") + e.what());
    }
  }
  if (include.empty()) {
    return fail("no_java_code", "dynamic tracing supports Java; project '" + project + "' has no indexed Java code");
  }
  // Remember the configuration so `proj start dynamic` (all) can start it again.
  json cfg = config();
  json& entry = cfg["projects"][project];
  if (entry.is_null()) entry = json::object();
  if (!match.empty()) entry["match"] = match;
  if (include_override.is_array() && !include_override.empty()) entry["include"] = include_override;
  save_config(cfg);

  auto existing = monitors_.find(project);
  bool already = existing != monitors_.end() && !existing->second.stopping;
  if (existing != monitors_.end() && existing->second.stopping) {
    return fail("stopping", "monitoring of '" + project + "' is being stopped; try again in a moment");
  }
  Monitor& m = monitors_[project];
  if (!already) {
    m.project = project;
    m.record = *rec;
    m.include = include;
    std::string inc;
    for (const auto& p : include) inc += (inc.empty() ? "" : ":") + p;
    m.agent_args = "project=" + project + ",service=" + service_file(opt_.data_dir).string() + ",include=" + inc;
    set_state(m, "IDLE", "waiting for a JVM started with the agent");
    // Opening the store marks sessions a previous service left open as interrupted.
    try {
      store(m);
    } catch (const std::exception& e) {
      monitors_.erase(project);
      return fail("store_unreadable", std::string("cannot open the runtime store: ") + e.what());
    }
    // Agents that connected while the project was not monitored start recording now.
    for (auto& [fd, c] : conns_) {
      if (c.role != Conn::Role::Agent || c.closed || c.project != project || c.session >= 0) continue;
      send(c, {{"type", "enable"}});
      set_state(m, "TARGET_DISCOVERED", "pid " + std::to_string(c.pid) + " was waiting on standby");
      json hello = c.hello;
      hello["since_start"] = false;  // recording starts now
      json context = {{"index_identity", m.record.last_success_at.value_or("")},
                      {"source_revision", platform::git_revision(m.record.root_path)}};
      c.session = store(m).open_session(hello, context);
      m.agents.insert(fd);
      set_state(m, "ATTACHED", "pid " + std::to_string(c.pid) + ": recording from now on (startup was not recorded)");
    }
    commit(m);
  }
  if (pid > 0 && pid != m.pid) {
    m.pid = pid;
    m.given_up.erase(pid);
    m.attempts = 0;
    m.next_attempt = Clock::now();
  }
  if (!match.empty()) m.match = match;
  result["ok"] = true;
  result["already"] = already;
  return true;
}

void Service::stop(Conn& c, const std::string& project) {
  had_command_ = true;
  std::set<std::string> targets;
  for (auto& [name, m] : monitors_) {
    if (!project.empty() && name != project) continue;
    targets.insert(name);
    if (m.stopping) continue;
    m.stopping = true;
    m.stop_deadline = Clock::now() + std::chrono::milliseconds(kStopTimeoutMs);
    set_state(m, "FLUSHING", "stopping: detaching agents and saving what they send");
    for (int fd : m.agents) {
      auto it = conns_.find(fd);
      if (it != conns_.end()) send(it->second, {{"type", "detach"}});
    }
    for (auto& a : attaches_) {
      if (a.project == name) a.deadline = Clock::now();  // cancel
    }
  }
  if (c.fd < 0) return;  // signal: nobody to answer
  if (targets.empty()) {
    json resp = {{"ok", true}, {"stopped", json::array()}};
    resp["note"] = project.empty() ? "no project was being monitored" : "'" + project + "' was not being monitored";
    resp.update(status_json(""));
    send(c, resp);
    return;
  }
  stop_waiters_.push_back({c.fd, targets});
}

void Service::finish_stop(Monitor& m) {
  commit(m);
  set_state(m, "STOPPED", "monitoring stopped");
  log::info("dynamic", m.project + ": monitoring stopped");
  m.store.reset();
}

void Service::answer_stop_waiters() {
  for (auto it = stop_waiters_.begin(); it != stop_waiters_.end();) {
    bool done = std::none_of(it->second.begin(), it->second.end(), [&](const std::string& p) { return monitors_.count(p) > 0; });
    if (!done) {
      ++it;
      continue;
    }
    auto conn = conns_.find(it->first);
    if (conn != conns_.end() && !conn->second.closed) {
      json resp = {{"ok", true}, {"stopped", json(it->second)}};
      resp.update(status_json(""));
      send(conn->second, resp);
    }
    it = stop_waiters_.erase(it);
  }
}

void Service::set_state(Monitor& m, const std::string& state, const std::string& detail) {
  if (m.state == state && !m.history.empty() && m.history.back().value("detail", std::string()) == detail) return;
  m.state = state;
  m.since = iso_now();
  m.history.push_back({{"state", state}, {"at", m.since}, {"detail", detail}});
  while (m.history.size() > kHistory) m.history.pop_front();
  log::info("dynamic", m.project + ": " + state + " (" + detail + ")");
}

}  // namespace

int run_service(const ServiceOptions& options) {
  log::configure(log::Level::Warn, options.data_dir / "logs" / "dynamic.log");
  Service service(options);
  try {
    return service.run();
  } catch (const std::exception& e) {
    log::error("dynamic", std::string("service failed: ") + e.what());
    std::fprintf(stderr, "proj-engine: dynamic service: %s\n", e.what());
    return 1;
  }
}

}  // namespace proj::dynamic

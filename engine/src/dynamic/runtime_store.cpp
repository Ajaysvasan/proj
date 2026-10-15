#include "dynamic/runtime_store.hpp"

#include <map>

#include "core/hash.hpp"
#include "core/time.hpp"

namespace proj::dynamic {
namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE rt_sessions (
  id               INTEGER PRIMARY KEY,
  agent_session    TEXT NOT NULL UNIQUE,
  pid              INTEGER,
  mode             TEXT NOT NULL,          -- startup | attach
  startup_coverage TEXT NOT NULL,          -- complete | attached_late | interrupted
  attach_delay_ms  INTEGER,                -- agent load time after JVM start
  agent_version    TEXT,
  jvm_version      TEXT,
  jvm_vendor       TEXT,
  include          TEXT,
  index_identity   TEXT,
  source_revision  TEXT,
  started_at       TEXT NOT NULL,
  ended_at         TEXT,
  status           TEXT NOT NULL,          -- capturing | ended | stopped | interrupted
  end_reason       TEXT,
  stats_json       TEXT NOT NULL DEFAULT '{}',
  traces           INTEGER NOT NULL DEFAULT 0,
  errors           INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE rt_methods (
  id          INTEGER PRIMARY KEY,
  class_name  TEXT NOT NULL,   -- binary name: com.acme.Foo$Bar
  name        TEXT NOT NULL,
  descriptor  TEXT NOT NULL,   -- JVM descriptor: tells overloads apart
  source_file TEXT,
  first_line  INTEGER,
  last_line   INTEGER,
  UNIQUE (class_name, name, descriptor)
);
CREATE INDEX rt_methods_source ON rt_methods(source_file);
CREATE TABLE rt_traces (
  id            INTEGER PRIMARY KEY,
  session_id    INTEGER NOT NULL REFERENCES rt_sessions(id),
  agent_trace   INTEGER NOT NULL,
  thread_id     INTEGER,
  thread_name   TEXT,
  root_method   INTEGER NOT NULL REFERENCES rt_methods(id),
  http_method   TEXT,
  http_path     TEXT,
  started_ms    INTEGER NOT NULL,
  duration_us   INTEGER,
  status        TEXT NOT NULL,   -- complete | truncated | open (snapshot) | incomplete (JVM exit, stop)
  final         INTEGER NOT NULL DEFAULT 0,
  node_count    INTEGER NOT NULL,
  dropped_nodes INTEGER NOT NULL DEFAULT 0,
  error_count   INTEGER NOT NULL DEFAULT 0,
  nodes         TEXT NOT NULL,   -- [[method, parent, calls, first_us, exits_by_exception], ...]
  UNIQUE (session_id, agent_trace)
);
CREATE INDEX rt_traces_recent ON rt_traces(id DESC);
CREATE INDEX rt_traces_root ON rt_traces(root_method);
CREATE TABLE rt_edges (
  caller     INTEGER NOT NULL,
  callee     INTEGER NOT NULL,
  calls      INTEGER NOT NULL,
  traces     INTEGER NOT NULL,
  first_seen TEXT NOT NULL,
  last_seen  TEXT NOT NULL,
  last_trace INTEGER,
  PRIMARY KEY (caller, callee)
) WITHOUT ROWID;
CREATE INDEX rt_edges_callee ON rt_edges(callee);
CREATE TABLE rt_errors (
  id             INTEGER PRIMARY KEY,
  fingerprint    TEXT NOT NULL UNIQUE,
  exception      TEXT NOT NULL,
  message        TEXT,
  state          TEXT NOT NULL,     -- CAUGHT | WRAPPED | ESCAPED | UNCAUGHT | UNKNOWN
  rethrown       INTEGER NOT NULL DEFAULT 0,
  wrapped_by     TEXT,
  throw_method   INTEGER,
  throw_line     INTEGER,
  caught_method  INTEGER,
  caught_line    INTEGER,
  caught_type    TEXT,
  escaped_from   INTEGER,
  thread_name    TEXT,
  frames         TEXT NOT NULL,     -- as thrown: deepest (most recent) frame first
  frames_omitted INTEGER NOT NULL DEFAULT 0,
  causes         TEXT NOT NULL DEFAULT '[]',
  suppressed     INTEGER NOT NULL DEFAULT 0,
  events         TEXT NOT NULL DEFAULT '[]',
  occurrences    INTEGER NOT NULL DEFAULT 1,
  first_seen     TEXT NOT NULL,
  last_seen      TEXT NOT NULL,
  last_session   INTEGER,
  last_trace     INTEGER
);
CREATE INDEX rt_errors_recent ON rt_errors(last_seen DESC);
CREATE TABLE rt_error_traces (
  error_id INTEGER NOT NULL,
  trace_id INTEGER NOT NULL,
  PRIMARY KEY (error_id, trace_id)
) WITHOUT ROWID;
CREATE INDEX rt_error_traces_trace ON rt_error_traces(trace_id);
)SQL";

bool is_final(const std::string& status) { return status == "complete" || status == "truncated" || status == "incomplete"; }

std::string text_or_empty(const json& j, const char* key) {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : "";
}

int64_t int_or(const json& j, const char* key, int64_t def) {
  return j.contains(key) && j[key].is_number_integer() ? j[key].get<int64_t>() : def;
}

}  // namespace

bool runtime_schema_current(db::Database& db) {
  return db.query_int("PRAGMA user_version").value_or(0) == RuntimeStore::kSchemaVersion;
}

void migrate_runtime(db::Database& db) {
  int64_t v = db.query_int("PRAGMA user_version").value_or(0);
  if (v == RuntimeStore::kSchemaVersion) return;
  if (v > RuntimeStore::kSchemaVersion) {
    throw db::Error(0, "runtime store has a newer schema (" + std::to_string(v) + ") than this engine supports");
  }
  db::Transaction tx(db);
  db.exec(kSchema);
  db.exec("PRAGMA user_version=" + std::to_string(RuntimeStore::kSchemaVersion));
  tx.commit();
}

RuntimeStore::RuntimeStore(const std::filesystem::path& path, Retention retention)
    : db_(path, db::OpenMode::ReadWriteCreate), retention_(retention) {
  db_.exec("PRAGMA journal_mode=WAL");     // readers (queries) never block the service
  db_.exec("PRAGMA synchronous=NORMAL");   // durable at checkpoints; a crash loses at most the last batch
  db_.exec("PRAGMA busy_timeout=5000");
  db_.exec("PRAGMA cache_size=-4096");
  migrate_runtime(db_);
}

void RuntimeStore::begin() {
  if (batch_) return;
  db_.exec("BEGIN IMMEDIATE");
  batch_ = true;
}

void RuntimeStore::commit() {
  if (!batch_) return;
  db_.exec("COMMIT");
  batch_ = false;
}

// ---- sessions -----------------------------------------------------------------

int64_t RuntimeStore::open_session(const json& hello, const json& context) {
  std::string agent_session = text_or_empty(hello, "session");
  if (agent_session.empty()) throw std::invalid_argument("hello without a session id");
  std::string mode = text_or_empty(hello, "mode") == "attach" ? "attach" : "startup";
  bool since_start = hello.value("since_start", false);
  const json& jvm = hello.contains("jvm") && hello["jvm"].is_object() ? hello["jvm"] : json::object();
  int64_t jvm_start = int_or(jvm, "start_ms", 0);
  int64_t loaded = int_or(hello, "loaded_ms", 0);
  std::string coverage = mode == "attach" ? "attached_late" : (since_start ? "complete" : "interrupted");

  auto find = db_.prepare("SELECT id FROM rt_sessions WHERE agent_session = ?");
  find.bind(1, agent_session);
  if (find.step()) {
    int64_t id = find.int64(0);
    find.reset();
    auto up = db_.prepare("UPDATE rt_sessions SET status = 'capturing', ended_at = NULL, end_reason = NULL WHERE id = ?");
    up.bind(1, id).run();
    return id;
  }
  find.reset();
  auto ins = db_.prepare(
      "INSERT INTO rt_sessions(agent_session, pid, mode, startup_coverage, attach_delay_ms, agent_version, jvm_version, "
      "jvm_vendor, include, index_identity, source_revision, started_at, status) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?, 'capturing')");
  ins.bind(1, agent_session).bind(2, int_or(hello, "pid", -1)).bind(3, mode).bind(4, coverage);
  if (jvm_start > 0 && loaded >= jvm_start) ins.bind(5, loaded - jvm_start); else ins.bind(5, nullptr);
  ins.bind_text_or_null(6, text_or_empty(hello, "agent")).bind_text_or_null(7, text_or_empty(jvm, "version"));
  ins.bind_text_or_null(8, text_or_empty(jvm, "vendor")).bind_text_or_null(9, text_or_empty(hello, "include"));
  ins.bind_text_or_null(10, text_or_empty(context, "index_identity"));
  ins.bind_text_or_null(11, text_or_empty(context, "source_revision"));
  ins.bind(12, iso_now()).run();
  return db_.last_insert_rowid();
}

void RuntimeStore::session_stats(int64_t session, const json& stats) {
  auto up = db_.prepare(
      "UPDATE rt_sessions SET stats_json = ?, startup_coverage = CASE WHEN startup_coverage = 'complete' "
      "AND (? = 0 OR ? > 0) THEN 'interrupted' ELSE startup_coverage END WHERE id = ?");
  bool since_start = stats.value("since_start", true);
  int64_t dropped = int_or(stats, "dropped_items", 0);
  up.bind(1, stats.dump()).bind(2, since_start ? 1 : 0).bind(3, dropped).bind(4, session).run();
}

void RuntimeStore::close_session(int64_t session, const std::string& status, const std::string& reason) {
  auto up = db_.prepare("UPDATE rt_sessions SET status = ?, end_reason = ?, ended_at = ? WHERE id = ? AND status = 'capturing'");
  up.bind(1, status).bind(2, reason).bind(3, iso_now()).bind(4, session).run();
  methods_.erase(session);
}

int64_t RuntimeStore::mark_interrupted(const std::string& reason) {
  auto up = db_.prepare("UPDATE rt_sessions SET status = 'interrupted', end_reason = ?, ended_at = ? WHERE status = 'capturing'");
  up.bind(1, reason).bind(2, iso_now()).run();
  return db_.changes();
}

// ---- methods ------------------------------------------------------------------

void RuntimeStore::method(int64_t session, const json& def) {
  std::string cls = text_or_empty(def, "class");
  std::string name = text_or_empty(def, "name");
  std::string desc = text_or_empty(def, "desc");
  if (cls.empty() || name.empty() || !def.contains("id") || !def["id"].is_number_integer()) return;
  auto ins = db_.prepare(
      "INSERT INTO rt_methods(class_name, name, descriptor, source_file, first_line, last_line) VALUES (?,?,?,?,?,?) "
      "ON CONFLICT(class_name, name, descriptor) DO UPDATE SET source_file = COALESCE(source_file, excluded.source_file), "
      "first_line = CASE WHEN excluded.first_line > 0 THEN excluded.first_line ELSE first_line END, "
      "last_line = CASE WHEN excluded.last_line > 0 THEN excluded.last_line ELSE last_line END");
  int64_t first = int_or(def, "first_line", 0);
  int64_t last = int_or(def, "last_line", 0);
  ins.bind(1, cls).bind(2, name).bind(3, desc).bind_text_or_null(4, text_or_empty(def, "source"));
  if (first > 0) ins.bind(5, first); else ins.bind(5, nullptr);
  if (last > 0) ins.bind(6, last); else ins.bind(6, nullptr);
  ins.run();
  auto sel = db_.prepare("SELECT id FROM rt_methods WHERE class_name = ? AND name = ? AND descriptor = ?");
  sel.bind(1, cls).bind(2, name).bind(3, desc);
  if (sel.step()) methods_[session][def["id"].get<int64_t>()] = sel.int64(0);
}

// A definition can be missing only if the agent misbehaved; keep the tree's shape with a placeholder.
int64_t RuntimeStore::method_id(int64_t session, int64_t agent_id) {
  auto& m = methods_[session];
  if (auto it = m.find(agent_id); it != m.end()) return it->second;
  json def = {{"id", agent_id}, {"class", "<unknown>"}, {"name", "#" + std::to_string(agent_id)}, {"desc", ""}};
  method(session, def);
  return m[agent_id];
}

json RuntimeStore::map_site(int64_t session, const json& site) {
  if (!site.is_object() || !site.contains("method") || !site["method"].is_number_integer()) return nullptr;
  json out = site;
  out["method"] = method_id(session, site["method"].get<int64_t>());
  return out;
}

// ---- traces -------------------------------------------------------------------

bool RuntimeStore::trace(int64_t session, const json& msg) {
  if (!msg.contains("id") || !msg["id"].is_number_integer() || !msg.contains("nodes") || !msg["nodes"].is_array() ||
      msg["nodes"].empty()) {
    return false;
  }
  int64_t agent_trace = msg["id"].get<int64_t>();
  std::string status = text_or_empty(msg, "status");
  if (status.empty()) status = "complete";
  bool final = is_final(status);

  auto existing = db_.prepare("SELECT id, final FROM rt_traces WHERE session_id = ? AND agent_trace = ?");
  existing.bind(1, session).bind(2, agent_trace);
  int64_t row = -1;
  bool was_final = false;
  if (existing.step()) {
    row = existing.int64(0);
    was_final = existing.int64(1) != 0;
  }
  existing.reset();
  if (was_final) return false;

  // Nodes with this store's method ids: [method, parent, calls, first_us, exits_by_exception].
  json nodes = json::array();
  std::vector<int64_t> methods;
  for (const auto& n : msg["nodes"]) {
    if (!n.is_array() || n.size() < 5) return false;
    int64_t m = method_id(session, n[0].get<int64_t>());
    int64_t parent = n[1].get<int64_t>();
    if (parent >= static_cast<int64_t>(methods.size())) return false;  // parents precede children
    methods.push_back(m);
    nodes.push_back({m, parent, n[2], n[3], n[4]});
  }
  const json& thread = msg.contains("thread") && msg["thread"].is_object() ? msg["thread"] : json::object();
  const json& http = msg.contains("http") && msg["http"].is_object() ? msg["http"] : json::object();
  int64_t error_count = final && msg.contains("errors") && msg["errors"].is_array() ? static_cast<int64_t>(msg["errors"].size()) : 0;
  db::Statement st = row < 0 ? db_.prepare(
                                   "INSERT INTO rt_traces(session_id, agent_trace, thread_id, thread_name, root_method, "
                                   "http_method, http_path, started_ms, duration_us, status, final, node_count, "
                                   "dropped_nodes, error_count, nodes) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")
                             : db_.prepare(
                                   "UPDATE rt_traces SET session_id = ?, agent_trace = ?, thread_id = ?, thread_name = ?, "
                                   "root_method = ?, http_method = ?, http_path = ?, started_ms = ?, duration_us = ?, "
                                   "status = ?, final = ?, node_count = ?, dropped_nodes = ?, error_count = ?, nodes = ? "
                                   "WHERE id = ?");
  st.bind(1, session).bind(2, agent_trace).bind(3, int_or(thread, "id", -1)).bind_text_or_null(4, text_or_empty(thread, "name"));
  st.bind(5, methods[0]).bind_text_or_null(6, text_or_empty(http, "method")).bind_text_or_null(7, text_or_empty(http, "path"));
  st.bind(8, int_or(msg, "start_ms", 0)).bind(9, int_or(msg, "duration_us", 0)).bind(10, status).bind(11, final ? 1 : 0);
  st.bind(12, static_cast<int64_t>(nodes.size())).bind(13, int_or(msg, "dropped_nodes", 0)).bind(14, error_count);
  st.bind(15, nodes.dump());
  if (row >= 0) st.bind(16, row);
  st.run();
  if (row < 0) row = db_.last_insert_rowid();
  if (!final) return true;

  // Counted once, when the trace is final.
  std::string now = iso_now();
  auto edge = db_.prepare(
      "INSERT INTO rt_edges(caller, callee, calls, traces, first_seen, last_seen, last_trace) VALUES (?,?,?,1,?,?,?) "
      "ON CONFLICT(caller, callee) DO UPDATE SET calls = calls + excluded.calls, traces = traces + 1, "
      "last_seen = excluded.last_seen, last_trace = excluded.last_trace");
  std::map<std::pair<int64_t, int64_t>, int64_t> calls;
  for (size_t i = 1; i < nodes.size(); ++i) {
    int64_t parent = nodes[i][1].get<int64_t>();
    if (parent < 0) continue;
    calls[{methods[static_cast<size_t>(parent)], methods[i]}] += nodes[i][2].get<int64_t>();
  }
  for (const auto& [pair, n] : calls) {
    edge.bind(1, pair.first).bind(2, pair.second).bind(3, n).bind(4, now).bind(5, now).bind(6, row).run();
  }
  if (msg.contains("errors") && msg["errors"].is_array()) {
    for (const auto& e : msg["errors"]) {
      if (e.is_object()) insert_error(session, row, text_or_empty(thread, "name"), map_error(session, e));
    }
  }
  auto count = db_.prepare("UPDATE rt_sessions SET traces = traces + 1 WHERE id = ?");
  count.bind(1, session).run();
  return true;
}

// ---- errors -------------------------------------------------------------------

// Agent method ids in an error (sites, escaped_from, events) -> this store's ids.
json RuntimeStore::map_error(int64_t session, json e) {
  e["throw"] = map_site(session, e.value("throw", json()));
  e["caught"] = map_site(session, e.value("caught", json()));
  e["escaped_from"] = e.contains("escaped_from") && e["escaped_from"].is_number_integer()
                          ? json(method_id(session, e["escaped_from"].get<int64_t>()))
                          : json();
  json events = json::array();
  if (e.contains("events") && e["events"].is_array()) {
    for (const auto& ev : e["events"]) {
      if (!ev.is_array() || ev.size() < 2 || !ev[1].is_number_integer()) continue;
      json mapped = ev;
      mapped[1] = method_id(session, ev[1].get<int64_t>());
      events.push_back(mapped);
    }
  }
  e["events"] = events;
  return e;
}

void RuntimeStore::insert_error(int64_t session, int64_t trace_id, const std::string& thread, const json& e) {
  std::string type = text_or_empty(e, "exception");
  std::string state = text_or_empty(e, "state");
  if (type.empty() || state.empty()) return;
  json thrown = e.value("throw", json());
  json caught = e.value("caught", json());
  json escaped_from = e.value("escaped_from", json());
  int64_t escaped = escaped_from.is_number_integer() ? escaped_from.get<int64_t>() : -1;
  json events = e.value("events", json::array());
  json frames = e.contains("frames") && e["frames"].is_array() ? e["frames"] : json::array();
  // Same exception type, fate, throw site and leading frames: one record, counted.
  Fnv1a64 h;
  h.update(type);
  h.update("|");
  h.update(state);
  h.update("|");
  if (thrown.is_object()) h.update(std::to_string(thrown["method"].get<int64_t>()) + ":" + std::to_string(thrown.value("line", 0)));
  for (size_t i = 0; i < frames.size() && i < 8; ++i) h.update("|" + frames[i].dump());
  std::string fingerprint = h.hex();

  std::string now = iso_now();
  auto up = db_.prepare(
      "INSERT INTO rt_errors(fingerprint, exception, message, state, rethrown, wrapped_by, throw_method, throw_line, "
      "caught_method, caught_line, caught_type, escaped_from, thread_name, frames, frames_omitted, causes, suppressed, "
      "events, occurrences, first_seen, last_seen, last_session, last_trace) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?) "
      "ON CONFLICT(fingerprint) DO UPDATE SET occurrences = occurrences + 1, message = excluded.message, "
      "last_seen = excluded.last_seen, last_session = excluded.last_session, last_trace = excluded.last_trace, "
      "thread_name = excluded.thread_name");
  up.bind(1, fingerprint).bind(2, type).bind_text_or_null(3, text_or_empty(e, "message")).bind(4, state);
  up.bind(5, e.value("rethrown", false) ? 1 : 0).bind_text_or_null(6, text_or_empty(e, "wrapped_by"));
  if (thrown.is_object()) up.bind(7, thrown["method"].get<int64_t>()).bind(8, int_or(thrown, "line", 0));
  else up.bind(7, nullptr).bind(8, nullptr);
  if (caught.is_object()) {
    up.bind(9, caught["method"].get<int64_t>()).bind(10, int_or(caught, "line", 0));
    up.bind_text_or_null(11, text_or_empty(caught, "type"));
  } else {
    up.bind(9, nullptr).bind(10, nullptr).bind(11, nullptr);
  }
  up.bind_id_or_null(12, escaped).bind_text_or_null(13, thread).bind(14, frames.dump());
  up.bind(15, int_or(e, "frames_omitted", 0));
  up.bind(16, (e.contains("causes") && e["causes"].is_array() ? e["causes"] : json::array()).dump());
  up.bind(17, int_or(e, "suppressed", 0)).bind(18, events.dump()).bind(19, now).bind(20, now).bind(21, session);
  up.bind_id_or_null(22, trace_id).run();
  if (trace_id >= 0) {
    auto link = db_.prepare("INSERT OR IGNORE INTO rt_error_traces(error_id, trace_id) SELECT id, ? FROM rt_errors WHERE fingerprint = ?");
    link.bind(1, trace_id).bind(2, fingerprint).run();
  }
  auto count = db_.prepare("UPDATE rt_sessions SET errors = errors + 1 WHERE id = ?");
  count.bind(1, session).run();
}

void RuntimeStore::uncaught(int64_t session, const json& msg) {
  if (!msg.contains("error") || !msg["error"].is_object()) return;
  json e = map_error(session, msg["error"]);
  e["state"] = "UNCAUGHT";
  int64_t trace_id = -1;
  if (msg.contains("trace") && msg["trace"].is_number_integer()) {
    auto t = db_.prepare("SELECT id FROM rt_traces WHERE session_id = ? AND agent_trace = ?");
    t.bind(1, session).bind(2, msg["trace"].get<int64_t>());
    if (t.step()) trace_id = t.int64(0);
  }
  const json& thread = msg.contains("thread") && msg["thread"].is_object() ? msg["thread"] : json::object();
  if (trace_id >= 0) {
    // The trace already recorded it as ESCAPED: that occurrence ends the thread, so it moves to
    // UNCAUGHT, keeping what the trace saw (throw site, handlers it passed).
    auto esc = db_.prepare(
        "SELECT e.id, e.occurrences, e.events, e.throw_method, e.throw_line, e.escaped_from, e.rethrown, "
        "e.caught_method, e.caught_line, e.caught_type FROM rt_errors e JOIN rt_error_traces l ON l.error_id = e.id "
        "WHERE l.trace_id = ? AND e.exception = ? AND e.state = 'ESCAPED' LIMIT 1");
    esc.bind(1, trace_id).bind(2, text_or_empty(e, "exception"));
    if (esc.step()) {
      int64_t id = esc.int64(0);
      int64_t occ = esc.int64(1);
      json events = json::parse(esc.text(2), nullptr, false);
      json thrown = esc.is_null(3) ? json() : json{{"method", esc.int64(3)}, {"line", esc.int64(4)}};
      int64_t escaped = esc.is_null(5) ? -1 : esc.int64(5);
      bool rethrown = esc.int64(6) != 0;
      json caught = esc.is_null(7) ? json()
                                   : json{{"method", esc.int64(7)}, {"line", esc.int64(8)},
                                          {"type", esc.is_null(9) ? json() : json(esc.text(9))}};
      esc.reset();
      auto unlink = db_.prepare("DELETE FROM rt_error_traces WHERE error_id = ? AND trace_id = ?");
      unlink.bind(1, id).bind(2, trace_id).run();
      if (occ <= 1) {
        auto del = db_.prepare("DELETE FROM rt_errors WHERE id = ?");
        del.bind(1, id).run();
      } else {
        auto dec = db_.prepare("UPDATE rt_errors SET occurrences = occurrences - 1 WHERE id = ?");
        dec.bind(1, id).run();
      }
      if (escaped >= 0) e["escaped_from"] = escaped;
      if (events.is_array()) e["events"] = events;
      e["throw"] = thrown;
      e["caught"] = caught;
      e["rethrown"] = rethrown;
      auto fix = db_.prepare("UPDATE rt_sessions SET errors = errors - 1 WHERE id = ?");
      fix.bind(1, session).run();
    } else {
      esc.reset();
    }
  }
  insert_error(session, trace_id, text_or_empty(thread, "name"), e);
}

// ---- retention ----------------------------------------------------------------

void RuntimeStore::retain() {
  bool own = !batch_;
  if (own) begin();
  auto sessions = db_.query_int("SELECT COUNT(*) FROM rt_sessions").value_or(0);
  if (sessions > retention_.max_sessions) {
    auto drop = db_.prepare(
        "SELECT id FROM rt_sessions WHERE status != 'capturing' ORDER BY id LIMIT ?");
    drop.bind(1, sessions - retention_.max_sessions);
    std::vector<int64_t> ids;
    while (drop.step()) ids.push_back(drop.int64(0));
    drop.reset();
    for (int64_t id : ids) {
      auto links = db_.prepare("DELETE FROM rt_error_traces WHERE trace_id IN (SELECT id FROM rt_traces WHERE session_id = ?)");
      links.bind(1, id).run();
      auto traces = db_.prepare("DELETE FROM rt_traces WHERE session_id = ?");
      traces.bind(1, id).run();
      auto s = db_.prepare("DELETE FROM rt_sessions WHERE id = ?");
      s.bind(1, id).run();
    }
  }
  auto traces = db_.query_int("SELECT COUNT(*) FROM rt_traces").value_or(0);
  if (traces > retention_.max_traces) {
    auto cut = db_.prepare("SELECT id FROM rt_traces ORDER BY id DESC LIMIT 1 OFFSET ?");
    cut.bind(1, retention_.max_traces - 1);
    if (cut.step()) {
      int64_t oldest_kept = cut.int64(0);
      cut.reset();
      auto links = db_.prepare("DELETE FROM rt_error_traces WHERE trace_id < ?");
      links.bind(1, oldest_kept).run();
      auto del = db_.prepare("DELETE FROM rt_traces WHERE id < ?");
      del.bind(1, oldest_kept).run();
    }
  }
  auto errors = db_.query_int("SELECT COUNT(*) FROM rt_errors").value_or(0);
  if (errors > retention_.max_errors) {
    auto del = db_.prepare(
        "DELETE FROM rt_errors WHERE id IN (SELECT id FROM rt_errors ORDER BY last_seen, id LIMIT ?)");
    del.bind(1, errors - retention_.max_errors).run();
    db_.exec("DELETE FROM rt_error_traces WHERE error_id NOT IN (SELECT id FROM rt_errors)");
  }
  if (own) commit();
}

}  // namespace proj::dynamic

#include "database.h"
#include "event_bus.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace {
constexpr size_t MAX_KEY = 64, MAX_VALUE = 1024;
constexpr size_t AUTO_SNAPSHOT_BYTES = 4 * 1024 * 1024;   // keep the WAL from growing forever

// Splits on whitespace; "double quotes" group words, backslash escapes the next char.
bool tokenize(const std::string& s, std::vector<std::string>& out) {
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= n) break;
        std::string tok;
        if (s[i] == '"') {
            ++i;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) ++i;
                tok += s[i++];
            }
            if (i >= n) return false;   // unbalanced quote
            ++i;
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(s[i]))) tok += s[i++];
        }
        out.push_back(tok);
    }
    return true;
}
Database::Result bad(const std::string& m) { return {false, "ERR " + m}; }
}  // namespace

Database::Database(const EngineConfig& cfg) : cfg_(cfg), engine_(std::make_unique<StorageEngine>(cfg)) {}

Database::Result Database::execute(const std::string& line) {
    std::vector<std::string> t;
    if (!tokenize(line, t)) return bad("unbalanced quote");
    if (t.empty()) return bad("empty command");
    std::string cmd = t[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return std::toupper(c); });

    std::shared_lock<std::shared_mutex> g(mtx_);
    StorageEngine& e = *engine_;

    if (cmd == "PING") return {true, "PONG"};
    if (cmd == "HELP")
        return {true, "SET key value [ttl_seconds] | GET key | DEL key | TTL key | SNAPSHOT | INFO | FLUSH | PING"};
    if (cmd == "SET") {
        if (t.size() < 3 || t.size() > 4) return bad("usage: SET key value [ttl_seconds]");
        if (t[1].size() > MAX_KEY) return bad("key too long (max 64 bytes)");
        if (t[2].size() > MAX_VALUE) return bad("value too long (max 1024 bytes)");
        int ttl = 0;
        if (t.size() == 4) {
            try { ttl = std::stoi(t[3]); } catch (...) { return bad("ttl must be a number of seconds"); }
            if (ttl < 1 || ttl > 86400) return bad("ttl must be between 1 and 86400 seconds");
        }
        e.set(t[1], t[2], ttl);
        if (e.walBytes() > AUTO_SNAPSHOT_BYTES) { g.unlock(); std::shared_lock<std::shared_mutex> g2(mtx_); engine_->createSnapshot(); }
        return {true, "OK"};
    }
    if (cmd == "GET") {
        if (t.size() != 2) return bad("usage: GET key");
        std::string v;
        return {true, e.get(t[1], v) ? v : "(nil)"};
    }
    if (cmd == "DEL") {
        if (t.size() != 2) return bad("usage: DEL key");
        return {true, e.del(t[1]) ? "Deleted" : "Key not found"};
    }
    if (cmd == "TTL") {
        if (t.size() != 2) return bad("usage: TTL key");
        long long ms = e.ttlMs(t[1]);
        if (ms == -2) return {true, "-2 (no such key)"};
        if (ms == -1) return {true, "-1 (no expiry)"};
        return {true, std::to_string((ms + 999) / 1000) + " seconds"};
    }
    if (cmd == "SNAPSHOT") return {true, "Snapshot saved (" + std::to_string(e.createSnapshot()) + " keys), WAL truncated"};
    if (cmd == "INFO") return {true, e.infoJson()};
    if (cmd == "FLUSH") { e.flushAll(); return {true, "OK (all data, WAL and snapshot cleared)"}; }
    return bad("unknown command '" + t[0] + "' (try HELP)");
}

std::string Database::stateJson() {
    std::shared_lock<std::shared_mutex> g(mtx_);
    return engine_->stateJson();
}

std::string Database::walJson(size_t n) {
    std::shared_lock<std::shared_mutex> g(mtx_);
    std::string o = "{\"lines\":[";
    auto lines = engine_->walTail(n);
    for (size_t i = 0; i < lines.size(); ++i) { if (i) o += ','; o += "\"" + jsonEscape(lines[i]) + "\""; }
    return o + "]}";
}

void Database::crashAndRecover() {
    std::unique_lock<std::shared_mutex> g(mtx_);
    EventBus::instance().emit("crash");
    engine_.reset();                                    // memory is gone; nothing is flushed or snapshotted
    engine_ = std::make_unique<StorageEngine>(cfg_);    // recovery: snapshot + WAL replay
}

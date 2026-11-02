#include "storage_engine.h"
#include "event_bus.h"
#include <filesystem>
#include <sstream>

namespace {
uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
EngineConfig normalize(EngineConfig c) {
    if (c.shards < 1) c.shards = 1;
    std::error_code ec;
    std::filesystem::create_directories(c.dataDir, ec);
    return c;
}
EventBus& bus() { return EventBus::instance(); }
}  // namespace

StorageEngine::StorageEngine(const EngineConfig& c)
    : cfg_(normalize(c)),
      shardMtx_(new std::mutex[static_cast<size_t>(normalize(c).shards)]),
      wal_(cfg_.dataDir + "/wal.log", cfg_.fsyncWal),
      snapshot_(cfg_.dataDir + "/snapshot.db"),
      startMs_(nowMs()) {
    for (int i = 0; i < cfg_.shards; ++i)
        shards_.push_back(std::make_unique<HashTable>(i, cfg_.capacityPerShard, &stats_));
    recover();
}

int StorageEngine::shardFor(const std::string& key) const { return static_cast<int>(fnv1a(key) % static_cast<uint32_t>(cfg_.shards)); }

void StorageEngine::route(const char* op, const std::string& key, int shard) {
    if (!bus().enabled()) return;
    bus().emit("route", jstr("op", op) + "," + jstr("key", key) + "," + jnum("hash", fnv1a(key)) + "," +
                            jnum("shard", shard) + "," + jnum("bucket", HashTable::bucketOf(key)));
}

void StorageEngine::set(const std::string& key, const std::string& value, int ttlSeconds) {
    std::shared_lock<std::shared_mutex> g(engineMtx_);
    int s = shardFor(key);
    long long exp = ttlSeconds > 0 ? nowMs() + ttlSeconds * 1000LL : 0;
    std::lock_guard<std::mutex> lk(shardMtx_[s]);
    route("SET", key, s);
    std::string line = wal_.appendSet(key, value, exp);          // 1) log first
    bus().emit("wal", jstr("line", line));
    shards_[s]->set(key, value, exp);                            // 2) then apply
}

bool StorageEngine::get(const std::string& key, std::string& value) {
    int s = shardFor(key);
    route("GET", key, s);
    return shards_[s]->get(key, value);
}

bool StorageEngine::del(const std::string& key) {
    std::shared_lock<std::shared_mutex> g(engineMtx_);
    int s = shardFor(key);
    std::lock_guard<std::mutex> lk(shardMtx_[s]);
    route("DEL", key, s);
    std::string line = wal_.appendDelete(key);
    bus().emit("wal", jstr("line", line));
    return shards_[s]->remove(key);
}

long long StorageEngine::ttlMs(const std::string& key) { return shards_[shardFor(key)]->ttlMs(key); }

size_t StorageEngine::totalKeys() {
    size_t n = 0;
    for (auto& s : shards_) n += s->size();
    return n;
}

size_t StorageEngine::createSnapshot() {
    std::unique_lock<std::shared_mutex> g(engineMtx_);   // no writer can sneak in between dump and WAL truncate
    std::vector<Entry> all;
    for (auto& s : shards_) { auto d = s->dump(); all.insert(all.end(), d.begin(), d.end()); }
    if (!snapshot_.save(all)) return 0;
    wal_.clear();
    bus().emit("snapshot", jnum("keys", static_cast<long long>(all.size())));
    bus().emit("wal_clear");
    return all.size();
}

void StorageEngine::flushAll() {
    std::unique_lock<std::shared_mutex> g(engineMtx_);
    for (auto& s : shards_) s->clear();
    wal_.clear();
    snapshot_.remove();
    bus().emit("flush");
    bus().emit("wal_clear");
}

void StorageEngine::recover() {
    long long t0 = nowMs();
    bus().emit("recover_start");
    for (const auto& e : snapshot_.load()) {
        if (e.expireAtMs && e.expireAtMs <= t0) continue;
        shards_[shardFor(e.key)]->set(e.key, e.value, e.expireAtMs);
        ++rec_.snapshotKeys;
    }
    bus().emit("recover_snapshot", jnum("keys", static_cast<long long>(rec_.snapshotKeys)));

    size_t torn = 0;
    auto lines = wal_.readAll(&torn);
    rec_.walEntries = lines.size();
    rec_.walSkipped = torn;
    for (const auto& line : lines) {
        std::istringstream iss(line);
        std::string cmd, k, v, e, key, val;
        iss >> cmd;
        if (cmd == "SET" && (iss >> k >> v >> e)) {
            long long exp = 0;
            try { exp = std::stoll(e); } catch (...) { ++rec_.walSkipped; continue; }
            if (!decodeToken(k, key) || !decodeToken(v, val)) { ++rec_.walSkipped; continue; }
            int s = shardFor(key);
            if (exp && exp <= t0) { shards_[s]->remove(key); ++rec_.walExpired; continue; }   // never resurrect expired keys
            shards_[s]->set(key, val, exp);
            ++rec_.walApplied;
        } else if (cmd == "DEL" && (iss >> k) && decodeToken(k, key)) {
            shards_[shardFor(key)]->remove(key);
            ++rec_.walApplied;
        } else {
            ++rec_.walSkipped;
        }
    }
    rec_.millis = nowMs() - t0;
    bus().emit("recover_wal", jnum("entries", static_cast<long long>(rec_.walEntries)) + "," +
                                  jnum("applied", static_cast<long long>(rec_.walApplied)) + "," +
                                  jnum("skipped", static_cast<long long>(rec_.walSkipped)) + "," +
                                  jnum("expired", static_cast<long long>(rec_.walExpired)));
    bus().emit("recover_done", jnum("keys", static_cast<long long>(totalKeys())) + "," + jnum("ms", rec_.millis));
}

std::string StorageEngine::infoJson() {
    auto n = [](const std::atomic<uint64_t>& a) { return static_cast<long long>(a.load()); };
    return "{" + jnum("sets", n(stats_.sets)) + "," + jnum("gets", n(stats_.gets)) + "," + jnum("hits", n(stats_.hits)) + "," +
           jnum("misses", n(stats_.misses)) + "," + jnum("dels", n(stats_.dels)) + "," + jnum("evictions", n(stats_.evictions)) + "," +
           jnum("expirations", n(stats_.expirations)) + "," + jnum("keys", static_cast<long long>(totalKeys())) + "," +
           jnum("shards", cfg_.shards) + "," + jnum("capacityPerShard", static_cast<long long>(cfg_.capacityPerShard)) + "," +
           jnum("walEntries", static_cast<long long>(wal_.entries())) + "," + jnum("walBytes", static_cast<long long>(wal_.bytes())) + "," +
           jnum("uptimeMs", nowMs() - startMs_) + ",\"recovery\":{" + jnum("snapshotKeys", static_cast<long long>(rec_.snapshotKeys)) + "," +
           jnum("walEntries", static_cast<long long>(rec_.walEntries)) + "," + jnum("walApplied", static_cast<long long>(rec_.walApplied)) + "," +
           jnum("ms", rec_.millis) + "}}";
}

std::string StorageEngine::stateJson() {
    std::string o = "{\"shards\":[";
    for (size_t i = 0; i < shards_.size(); ++i) { if (i) o += ','; o += shards_[i]->stateJson(); }
    return o + "],\"info\":" + infoJson() + "}";
}

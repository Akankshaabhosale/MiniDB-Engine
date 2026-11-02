// Functional tests for the guarantees the README claims. Exit code 0 = all passed.
#include "database.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::cerr << "  FAIL line " << __LINE__ << ": " #cond "\n"; } } while (0)

static EngineConfig cfgFor(const std::string& name, int shards = 4, size_t cap = 1000) {
    EngineConfig c;
    c.dataDir = "test_data_" + name;
    c.shards = shards;
    c.capacityPerShard = cap;
    return c;
}
static void fresh(const EngineConfig& c) { std::error_code ec; fs::remove_all(c.dataDir, ec); }
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::string get(StorageEngine& e, const std::string& k) { std::string v; return e.get(k, v) ? v : "<nil>"; }

static void testBasic() {
    std::cout << "basic set/get/del/overwrite\n";
    auto c = cfgFor("basic"); fresh(c);
    StorageEngine e(c);
    e.set("a", "1"); e.set("b", "2");
    CHECK(get(e, "a") == "1" && get(e, "b") == "2" && get(e, "zzz") == "<nil>");
    e.set("a", "9");
    CHECK(get(e, "a") == "9");
    CHECK(e.del("a") && !e.del("a") && get(e, "a") == "<nil>");
}

static void testTTL() {
    std::cout << "ttl expiry (lazy + background cleaner)\n";
    auto c = cfgFor("ttl"); fresh(c);
    StorageEngine e(c);
    e.set("k", "v", 1);
    CHECK(get(e, "k") == "v");
    long long left = e.ttlMs("k");
    CHECK(left > 0 && left <= 1000);
    sleepMs(1500);
    CHECK(e.totalKeys() == 0);          // removed by the cleaner without anybody reading it
    CHECK(get(e, "k") == "<nil>");
}

static void testLRU() {
    std::cout << "LRU eviction order\n";
    auto c = cfgFor("lru", 1, 2); fresh(c);
    StorageEngine e(c);
    e.set("a", "1"); e.set("b", "2");
    get(e, "a");                         // a is now most recent, b is the LRU victim
    e.set("c", "3");
    CHECK(get(e, "b") == "<nil>" && get(e, "a") == "1" && get(e, "c") == "3");
}

static void testWalRecovery() {
    std::cout << "crash recovery from WAL only\n";
    auto c = cfgFor("wal"); fresh(c);
    { StorageEngine e(c); e.set("x", "hello world"); e.set("y", "2"); e.set("z", "3"); e.del("y"); }   // no snapshot
    StorageEngine e2(c);
    CHECK(get(e2, "x") == "hello world");   // value with a space survives
    CHECK(get(e2, "y") == "<nil>" && get(e2, "z") == "3");
    CHECK(e2.recoveryInfo().walEntries == 4);
}

static void testSnapshotPlusWal() {
    std::cout << "snapshot + later WAL entries\n";
    auto c = cfgFor("snap"); fresh(c);
    {
        StorageEngine e(c);
        e.set("a", "1"); e.set("b", "2");
        CHECK(e.createSnapshot() == 2);
        CHECK(e.walBytes() == 0);        // WAL truncated by the snapshot
        e.set("c", "3"); e.del("a");
    }
    StorageEngine e2(c);
    CHECK(e2.recoveryInfo().snapshotKeys == 2);
    CHECK(get(e2, "a") == "<nil>" && get(e2, "b") == "2" && get(e2, "c") == "3");
}

static void testExpiredNotResurrected() {
    std::cout << "expired keys stay dead after restart\n";
    auto c = cfgFor("exp"); fresh(c);
    { StorageEngine e(c); e.set("old", "v", 1); e.set("forever", "v"); }
    sleepMs(1300);
    StorageEngine e2(c);
    CHECK(get(e2, "old") == "<nil>" && get(e2, "forever") == "v");
}

static void testTornWal() {
    std::cout << "torn final WAL line is ignored\n";
    auto c = cfgFor("torn"); fresh(c);
    { StorageEngine e(c); e.set("good", "1"); }
    { std::ofstream out(c.dataDir + "/wal.log", std::ios::app | std::ios::binary); out << "SET half_writ"; }   // no newline
    StorageEngine e2(c);
    CHECK(get(e2, "good") == "1" && get(e2, "half_writ") == "<nil>");
}

static void testCommands() {
    std::cout << "command parser (quotes, errors, limits)\n";
    auto c = cfgFor("cmd"); fresh(c);
    Database db(c);
    CHECK(db.execute("SET name \"Ada Lovelace\"").ok);
    CHECK(db.execute("GET name").reply == "Ada Lovelace");
    CHECK(db.execute("get missing").reply == "(nil)");
    CHECK(!db.execute("SET onlykey").ok);
    CHECK(!db.execute("SET k v notanumber").ok);
    CHECK(!db.execute("SET k \"unterminated").ok);
    CHECK(!db.execute(std::string("SET ") + std::string(100, 'k') + " v").ok);
    CHECK(!db.execute("BOGUS").ok);
    db.execute("SET keep me");
    db.crashAndRecover();
    CHECK(db.execute("GET keep").reply == "me");
}

int main() {
    testBasic(); testTTL(); testLRU(); testWalRecovery(); testSnapshotPlusWal();
    testExpiredNotResurrected(); testTornWal(); testCommands();
    std::error_code ec;
    for (auto& p : fs::directory_iterator(".", ec)) if (p.path().filename().string().rfind("test_data_", 0) == 0) fs::remove_all(p.path(), ec);
    std::cout << (checks - failures) << "/" << checks << " checks passed\n";
    return failures ? 1 : 0;
}

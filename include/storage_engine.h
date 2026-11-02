#pragma once
#include "hash_table.h"
#include "snapshot_manager.h"
#include "write_ahead_log.h"
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

struct EngineConfig {
    int shards = 4;
    size_t capacityPerShard = 100000;
    std::string dataDir = "data";
    bool fsyncWal = false;
};

struct RecoveryInfo {
    size_t snapshotKeys = 0, walEntries = 0, walApplied = 0, walSkipped = 0, walExpired = 0;
    long long millis = 0;
};

class StorageEngine {
public:
    explicit StorageEngine(const EngineConfig& cfg);

    void set(const std::string& key, const std::string& value, int ttlSeconds = 0);
    bool get(const std::string& key, std::string& value);
    bool del(const std::string& key);
    long long ttlMs(const std::string& key);
    size_t createSnapshot();     // dump all shards, then truncate the WAL (atomic w.r.t. writers)
    void flushAll();
    size_t totalKeys();

    std::string stateJson();
    std::string infoJson();
    std::vector<std::string> walTail(size_t n) { return wal_.tail(n); }
    size_t walBytes() { return wal_.bytes(); }
    const RecoveryInfo& recoveryInfo() const { return rec_; }

private:
    EngineConfig cfg_;
    Stats stats_;
    std::vector<std::unique_ptr<HashTable>> shards_;
    std::unique_ptr<std::mutex[]> shardMtx_;   // makes "append WAL + apply" one ordered step per shard
    WriteAheadLog wal_;
    SnapshotManager snapshot_;
    std::shared_mutex engineMtx_;              // writers share it; snapshot/flush take it exclusively
    RecoveryInfo rec_;
    long long startMs_;

    int shardFor(const std::string& key) const;
    void route(const char* op, const std::string& key, int shard);
    void recover();
};

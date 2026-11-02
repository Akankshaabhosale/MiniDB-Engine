#pragma once
// Command layer shared by the TCP and HTTP front-ends.
#include "storage_engine.h"
#include <memory>
#include <shared_mutex>
#include <string>

class Database {
public:
    struct Result { bool ok; std::string reply; };

    explicit Database(const EngineConfig& cfg);
    Result execute(const std::string& line);
    std::string stateJson();
    std::string walJson(size_t n = 40);
    void crashAndRecover();   // drops the in-memory state WITHOUT a snapshot, then recovers from disk

private:
    EngineConfig cfg_;
    std::shared_mutex mtx_;
    std::unique_ptr<StorageEngine> engine_;
};

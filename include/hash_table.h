#pragma once
#include "common.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct Stats {
    std::atomic<uint64_t> sets{0}, gets{0}, hits{0}, misses{0}, dels{0}, evictions{0}, expirations{0};
};

// One shard: chained hash table (djb2) + doubly linked LRU list + background TTL cleaner.
// A single mutex guards the shard (reads also reorder the LRU list, so they are writes too).
class HashTable {
public:
    static constexpr int TABLE_SIZE = 101;

    HashTable(int shardId, size_t capacity, Stats* stats);
    ~HashTable();
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    void set(const std::string& key, const std::string& value, long long expireAtMs = 0);
    bool get(const std::string& key, std::string& value);
    bool remove(const std::string& key);
    long long ttlMs(const std::string& key);   // -2 missing, -1 no expiry
    void clear();
    size_t size();
    std::vector<Entry> dump();                 // non-expired entries (for snapshots)
    std::string stateJson();                   // buckets + LRU order (for the visualizer)
    static int bucketOf(const std::string& key);

private:
    struct Node {
        std::string key, value;
        long long expireAtMs = 0;
        Node* next = nullptr;                  // bucket chain
        Node* prevLRU = nullptr;
        Node* nextLRU = nullptr;
    };

    int shardId_;
    size_t capacity_;
    Stats* stats_;
    std::vector<Node*> table_;
    Node* head_ = nullptr;                     // most recently used
    Node* tail_ = nullptr;                     // least recently used
    size_t size_ = 0;
    bool running_ = true;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread cleaner_;

    void addToFront(Node* n);
    void removeFromLRU(Node* n);
    void moveToFront(Node* n);
    void removeNode(Node* n);                  // unlink from bucket + LRU, delete
    void evictLRU();
    void cleanerLoop();
    void ev(const char* type, const std::string& key, const std::string& extra = "");
};

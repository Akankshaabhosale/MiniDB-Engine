#include "hash_table.h"
#include "event_bus.h"
#include <algorithm>

int HashTable::bucketOf(const std::string& key) {
    uint64_t h = 5381;                       // djb2
    for (unsigned char c : key) h = h * 33 + c;
    return static_cast<int>(h % TABLE_SIZE);
}

HashTable::HashTable(int shardId, size_t capacity, Stats* stats)
    : shardId_(shardId), capacity_(capacity ? capacity : 1), stats_(stats), table_(TABLE_SIZE, nullptr) {
    cleaner_ = std::thread(&HashTable::cleanerLoop, this);
}

HashTable::~HashTable() {
    { std::lock_guard<std::mutex> lk(mtx_); running_ = false; }
    cv_.notify_all();
    cleaner_.join();
    for (Node* n = head_; n;) { Node* nx = n->nextLRU; delete n; n = nx; }
}

void HashTable::ev(const char* type, const std::string& key, const std::string& extra) {
    EventBus& bus = EventBus::instance();
    if (!bus.enabled()) return;
    std::string f = jnum("shard", shardId_) + "," + jstr("key", key);
    if (!extra.empty()) f += "," + extra;
    bus.emit(type, f);
}

void HashTable::addToFront(Node* n) {
    n->prevLRU = nullptr;
    n->nextLRU = head_;
    if (head_) head_->prevLRU = n;
    head_ = n;
    if (!tail_) tail_ = n;
}

void HashTable::removeFromLRU(Node* n) {
    if (n->prevLRU) n->prevLRU->nextLRU = n->nextLRU; else head_ = n->nextLRU;
    if (n->nextLRU) n->nextLRU->prevLRU = n->prevLRU; else tail_ = n->prevLRU;
}

void HashTable::moveToFront(Node* n) {
    if (n == head_) return;
    removeFromLRU(n);
    addToFront(n);
}

void HashTable::removeNode(Node* n) {
    Node** pp = &table_[bucketOf(n->key)];
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    removeFromLRU(n);
    delete n;
    --size_;
}

void HashTable::evictLRU() {
    if (!tail_) return;
    ev("evict", tail_->key, jnum("bucket", bucketOf(tail_->key)));
    if (stats_) stats_->evictions++;
    removeNode(tail_);
}

void HashTable::set(const std::string& key, const std::string& value, long long expireAtMs) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (stats_) stats_->sets++;
    int b = bucketOf(key);
    for (Node* n = table_[b]; n; n = n->next) {
        if (n->key == key) {
            n->value = value;
            n->expireAtMs = expireAtMs;       // like Redis: a plain SET clears any old TTL
            moveToFront(n);
            ev("update", key, jnum("bucket", b) + "," + jnum("size", size_));
            return;
        }
    }
    if (size_ >= capacity_) evictLRU();
    Node* n = new Node{key, value, expireAtMs, table_[b], nullptr, nullptr};
    table_[b] = n;
    addToFront(n);
    ++size_;
    ev("insert", key, jnum("bucket", b) + "," + jnum("size", size_) + "," + jnum("capacity", capacity_));
}

bool HashTable::get(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (stats_) stats_->gets++;
    int b = bucketOf(key);
    for (Node* n = table_[b]; n; n = n->next) {
        if (n->key != key) continue;
        if (n->expireAtMs && nowMs() >= n->expireAtMs) {   // lazy expiry on read
            ev("expire", key, jnum("bucket", b) + ",\"how\":\"lazy\"");
            if (stats_) { stats_->expirations++; stats_->misses++; }
            removeNode(n);
            return false;
        }
        value = n->value;
        moveToFront(n);
        if (stats_) stats_->hits++;
        ev("get", key, jnum("bucket", b) + ",\"hit\":true");
        return true;
    }
    if (stats_) stats_->misses++;
    ev("get", key, jnum("bucket", b) + ",\"hit\":false");
    return false;
}

bool HashTable::remove(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    int b = bucketOf(key);
    for (Node* n = table_[b]; n; n = n->next) {
        if (n->key != key) continue;
        if (stats_) stats_->dels++;
        ev("delete", key, jnum("bucket", b));
        removeNode(n);
        return true;
    }
    return false;
}

long long HashTable::ttlMs(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Node* n = table_[bucketOf(key)]; n; n = n->next) {
        if (n->key != key) continue;
        if (!n->expireAtMs) return -1;
        long long left = n->expireAtMs - nowMs();
        if (left <= 0) {
            ev("expire", key, jnum("bucket", bucketOf(key)) + ",\"how\":\"lazy\"");
            if (stats_) stats_->expirations++;
            removeNode(n);
            return -2;
        }
        return left;
    }
    return -2;
}

void HashTable::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (Node* n = head_; n;) { Node* nx = n->nextLRU; delete n; n = nx; }
    head_ = tail_ = nullptr;
    size_ = 0;
    std::fill(table_.begin(), table_.end(), nullptr);
}

size_t HashTable::size() {
    std::lock_guard<std::mutex> lk(mtx_);
    return size_;
}

std::vector<Entry> HashTable::dump() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Entry> out;
    long long now = nowMs();
    for (Node* n = head_; n; n = n->nextLRU)
        if (!n->expireAtMs || n->expireAtMs > now) out.push_back({n->key, n->value, n->expireAtMs});
    return out;
}

std::string HashTable::stateJson() {
    std::lock_guard<std::mutex> lk(mtx_);
    long long now = nowMs();
    std::string o = "{" + jnum("id", shardId_) + "," + jnum("size", size_) + "," + jnum("capacity", capacity_) + ",\"buckets\":[";
    bool first = true;
    for (int i = 0; i < TABLE_SIZE; ++i) {
        if (!table_[i]) continue;
        if (!first) o += ',';
        first = false;
        o += "{" + jnum("i", i) + ",\"keys\":[";
        bool f2 = true;
        for (Node* n = table_[i]; n; n = n->next) {
            if (!f2) o += ',';
            f2 = false;
            o += "\"" + jsonEscape(n->key) + "\"";
        }
        o += "]}";
    }
    o += "],\"lru\":[";
    first = true;
    for (Node* n = head_; n; n = n->nextLRU) {
        if (!first) o += ',';
        first = false;
        long long ttl = n->expireAtMs ? std::max(0LL, n->expireAtMs - now) : -1;
        o += "{" + jstr("key", n->key) + "," + jstr("value", n->value.substr(0, 40)) + "," + jnum("ttlMs", ttl) + "}";
    }
    return o + "]}";
}

void HashTable::cleanerLoop() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (running_) {
        cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return !running_; });
        if (!running_) break;
        long long now = nowMs();
        for (int i = 0; i < TABLE_SIZE; ++i) {
            for (Node* n = table_[i]; n;) {
                Node* nx = n->next;
                if (n->expireAtMs && now >= n->expireAtMs) {
                    ev("expire", n->key, jnum("bucket", i) + ",\"how\":\"cleaner\"");
                    if (stats_) stats_->expirations++;
                    removeNode(n);
                }
                n = nx;
            }
        }
    }
}

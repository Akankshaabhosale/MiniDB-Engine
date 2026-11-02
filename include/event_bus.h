#pragma once
// Thread-safe ring buffer of engine events. The HTTP server streams these to the browser (SSE),
// which is how the web UI can show what happens inside the engine.
#include "common.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

class EventBus {
public:
    static EventBus& instance();
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // fields = already-built JSON members, e.g.  "shard":2,"key":"a"
    void emit(const std::string& type, const std::string& fields = "");
    // Blocks up to timeoutMs for events newer than lastId; updates lastId.
    std::vector<std::string> wait(uint64_t& lastId, int timeoutMs);
    uint64_t lastId();

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, std::string>> buf_;
    uint64_t nextId_ = 1;
    std::atomic<bool> enabled_{false};
};

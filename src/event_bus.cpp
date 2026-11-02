#include "event_bus.h"

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

void EventBus::emit(const std::string& type, const std::string& fields) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(m_);
    uint64_t id = nextId_++;
    std::string json = "{" + jnum("id", static_cast<long long>(id)) + "," + jnum("t", nowMs()) + "," + jstr("ev", type);
    if (!fields.empty()) json += "," + fields;
    json += "}";
    buf_.emplace_back(id, std::move(json));
    if (buf_.size() > 1000) buf_.pop_front();
    cv_.notify_all();
}

std::vector<std::string> EventBus::wait(uint64_t& lastId, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                 [&] { return !buf_.empty() && buf_.back().first > lastId; });
    std::vector<std::string> out;
    for (auto& p : buf_)
        if (p.first > lastId) { out.push_back(p.second); lastId = p.first; }
    return out;
}

uint64_t EventBus::lastId() {
    std::lock_guard<std::mutex> lk(m_);
    return nextId_ - 1;
}

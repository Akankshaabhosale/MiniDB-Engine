#pragma once
#include "database.h"
#include "platform.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

// Minimal HTTP/1.1 server (no dependencies): static page, JSON API and a Server-Sent-Events stream.
class HttpServer {
public:
    HttpServer(int port, Database& db, std::string webDir)
        : port_(port), db_(db), webDir_(std::move(webDir)) {}
    void start();   // blocking accept loop
private:
    int port_;
    Database& db_;
    std::string webDir_;
    std::atomic<int> sse_{0};
    std::mutex rlMtx_;
    std::map<std::string, std::deque<long long>> hits_;
    std::atomic<long long> lastCrash_{0};

    void handle(socket_t c, std::string ip);
    void serveEvents(socket_t c);
    bool allow(const std::string& ip);
    void reply(socket_t c, int status, const std::string& type, const std::string& body);
};

#pragma once
#include "database.h"
#include <atomic>

// Line-based text protocol (use telnet / netcat). One thread per connection.
class TCPServer {
public:
    TCPServer(int port, Database& db) : port_(port), db_(db) {}
    void start();   // blocking accept loop
private:
    int port_;
    Database& db_;
    std::atomic<int> clients_{0};
    void handleClient(int fd);
};

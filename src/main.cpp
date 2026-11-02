#include "database.h"
#include "event_bus.h"
#include "http_server.h"
#include "platform.h"
#include "tcp_server.h"
#include <cstdlib>
#include <iostream>
#include <thread>

static std::string env(const char* k, const std::string& d) {
    const char* v = std::getenv(k);
    return (v && *v) ? v : d;
}

int main() {
    netInit();
    EngineConfig cfg;
    cfg.dataDir = env("MINIDB_DATA_DIR", "data");
    cfg.shards = std::stoi(env("MINIDB_SHARDS", "4"));
    cfg.capacityPerShard = std::stoul(env("MINIDB_CAPACITY", "4"));   // tiny on purpose: makes LRU eviction easy to see
    cfg.fsyncWal = env("MINIDB_FSYNC", "0") == "1";
    int httpPort = std::stoi(env("PORT", env("MINIDB_HTTP_PORT", "8081")));   // PORT is what cloud hosts inject
    int tcpPort = std::stoi(env("MINIDB_TCP_PORT", "8080"));                  // 0 disables the raw TCP server

    EventBus::instance().setEnabled(true);
    Database db(cfg);
    std::cout << "[engine] " << cfg.shards << " shards x " << cfg.capacityPerShard << " keys, data dir '" << cfg.dataDir << "'\n";

    std::thread tcpThread;
    TCPServer tcp(tcpPort, db);
    if (tcpPort > 0) tcpThread = std::thread([&] { tcp.start(); });

    HttpServer http(httpPort, db, env("MINIDB_WEB_DIR", "web"));
    http.start();
    if (tcpThread.joinable()) tcpThread.join();
}

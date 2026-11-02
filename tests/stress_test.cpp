// Multi-threaded stress + correctness test: N threads hammer the engine, then we verify every value,
// then restart the engine from disk and verify again. Usage: StressTest [threads] [opsPerThread]
#include "storage_engine.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    int ops = argc > 2 ? std::atoi(argv[2]) : 5000;
    EngineConfig cfg;
    cfg.dataDir = "stress_data";
    cfg.capacityPerShard = 1000000;           // big enough that nothing is evicted, so we can verify everything
    std::error_code ec;
    std::filesystem::remove_all(cfg.dataDir, ec);

    std::vector<std::vector<double>> lat(threads);
    std::atomic<int> errors{0};
    auto t0 = Clock::now();
    {
        StorageEngine e(cfg);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
            lat[t].reserve(static_cast<size_t>(ops) * 2);
            for (int i = 0; i < ops; ++i) {
                std::string k = "t" + std::to_string(t) + ":k" + std::to_string(i), v = "v" + std::to_string(t) + ":" + std::to_string(i);
                auto a = Clock::now();
                e.set(k, v);
                auto b = Clock::now();
                std::string got;
                if (!e.get(k, got) || got != v) ++errors;      // read-your-own-write
                auto c = Clock::now();
                e.set("shared:k" + std::to_string(i % 50), v);   // contended keys: just must not crash/corrupt
                lat[t].push_back(std::chrono::duration<double, std::micro>(b - a).count());
                lat[t].push_back(std::chrono::duration<double, std::micro>(c - b).count());
            }
        });
        for (auto& th : pool) th.join();
        double secs = std::chrono::duration<double>(Clock::now() - t0).count();

        std::vector<double> all;
        for (auto& l : lat) all.insert(all.end(), l.begin(), l.end());
        std::sort(all.begin(), all.end());
        long long total = static_cast<long long>(threads) * ops * 3;
        std::cout << threads << " threads x " << ops << " iterations (" << total << " ops) in " << secs << " s\n"
                  << "throughput: " << static_cast<long long>(total / secs) << " ops/s (WAL flushed on every write)\n"
                  << "latency  p50: " << all[all.size() / 2] << " us   p99: " << all[all.size() * 99 / 100] << " us\n";

        for (int t = 0; t < threads; ++t)
            for (int i = 0; i < ops; ++i) {
                std::string got;
                if (!e.get("t" + std::to_string(t) + ":k" + std::to_string(i), got) || got != "v" + std::to_string(t) + ":" + std::to_string(i)) ++errors;
            }
        std::cout << "verify after load:      " << (errors ? "FAILED" : "ok") << " (" << errors << " errors)\n";
        if (!errors) e.createSnapshot();
        for (int t = 0; t < threads; ++t) e.set("after:" + std::to_string(t), "x");   // goes to WAL after the snapshot
    }
    StorageEngine again(cfg);                  // simulate restart: snapshot + WAL replay
    int missing = 0;
    std::string got;
    for (int t = 0; t < threads; ++t) {
        if (!again.get("after:" + std::to_string(t), got)) ++missing;
        for (int i = 0; i < ops; i += 7)
            if (!again.get("t" + std::to_string(t) + ":k" + std::to_string(i), got) || got != "v" + std::to_string(t) + ":" + std::to_string(i)) ++missing;
    }
    std::cout << "verify after restart:   " << (missing ? "FAILED" : "ok") << " (" << missing << " missing/wrong), recovery took "
              << again.recoveryInfo().millis << " ms for " << again.totalKeys() << " keys\n";
    std::filesystem::remove_all(cfg.dataDir, ec);
    return (errors || missing) ? 1 : 0;
}

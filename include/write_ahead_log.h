#pragma once
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Append-only log. Every mutation is written (and flushed) BEFORE it is applied in memory.
// Line format:  SET <key> <value> <expireAtMs>   |   DEL <key>     (tokens are percent-encoded)
class WriteAheadLog {
public:
    WriteAheadLog(const std::string& path, bool fsyncEveryWrite);
    ~WriteAheadLog();
    std::string appendSet(const std::string& key, const std::string& value, long long expireAtMs);
    std::string appendDelete(const std::string& key);
    // Returns only complete lines; a torn final line (crash mid-write) is counted in *torn and dropped.
    std::vector<std::string> readAll(size_t* torn = nullptr);
    std::vector<std::string> tail(size_t n);
    void clear();
    size_t entries();
    size_t bytes();

private:
    std::string path_;
    bool fsync_;
    std::mutex mtx_;
    FILE* fp_ = nullptr;
    size_t entries_ = 0, bytes_ = 0;
    std::string write(const std::string& line);
};

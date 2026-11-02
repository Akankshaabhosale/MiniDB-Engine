#include "write_ahead_log.h"
#include "common.h"
#include <fstream>
#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

WriteAheadLog::WriteAheadLog(const std::string& path, bool fsyncEveryWrite) : path_(path), fsync_(fsyncEveryWrite) {
    size_t torn = 0;
    entries_ = readAll(&torn).size();
    {   // measure existing size
        std::ifstream in(path_, std::ios::binary | std::ios::ate);
        bytes_ = in ? static_cast<size_t>(in.tellg()) : 0;
    }
    fp_ = std::fopen(path_.c_str(), "ab");
}

WriteAheadLog::~WriteAheadLog() { if (fp_) std::fclose(fp_); }

std::string WriteAheadLog::write(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!fp_) return line;
    std::string withNl = line + "\n";
    std::fwrite(withNl.data(), 1, withNl.size(), fp_);
    std::fflush(fp_);                          // data reaches the OS before we touch memory
    if (fsync_) {
#ifdef _WIN32
        _commit(_fileno(fp_));
#else
        fsync(fileno(fp_));
#endif
    }
    ++entries_;
    bytes_ += withNl.size();
    return line;
}

std::string WriteAheadLog::appendSet(const std::string& key, const std::string& value, long long expireAtMs) {
    return write("SET " + encodeToken(key) + " " + encodeToken(value) + " " + std::to_string(expireAtMs));
}

std::string WriteAheadLog::appendDelete(const std::string& key) { return write("DEL " + encodeToken(key)); }

std::vector<std::string> WriteAheadLog::readAll(size_t* torn) {
    std::vector<std::string> lines;
    if (torn) *torn = 0;
    std::ifstream in(path_, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (in.eof()) { if (torn) ++*torn; break; }   // last line had no '\n' => torn write, ignore it
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> WriteAheadLog::tail(size_t n) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto all = readAll();
    if (all.size() > n) all.erase(all.begin(), all.end() - static_cast<long>(n));
    return all;
}

void WriteAheadLog::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) std::fclose(fp_);
    fp_ = std::fopen(path_.c_str(), "wb");     // truncate
    if (fp_) { std::fclose(fp_); fp_ = std::fopen(path_.c_str(), "ab"); }
    entries_ = bytes_ = 0;
}

size_t WriteAheadLog::entries() { std::lock_guard<std::mutex> lk(mtx_); return entries_; }
size_t WriteAheadLog::bytes() { std::lock_guard<std::mutex> lk(mtx_); return bytes_; }

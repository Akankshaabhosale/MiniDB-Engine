#include "snapshot_manager.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

bool SnapshotManager::save(const std::vector<Entry>& entries) {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out << "MINIDB_SNAPSHOT 1\n";
        for (const auto& e : entries)
            out << encodeToken(e.key) << ' ' << encodeToken(e.value) << ' ' << e.expireAtMs << '\n';
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);   // atomic replace
    if (ec) { std::filesystem::remove(path_, ec); std::filesystem::rename(tmp, path_, ec); }
    return !ec;
}

std::vector<Entry> SnapshotManager::load() {
    std::vector<Entry> out;
    std::ifstream in(path_, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("MINIDB_SNAPSHOT", 0) != 0) return out;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string k, v, e;
        if (!(iss >> k >> v >> e)) continue;
        Entry en;
        try { en.expireAtMs = std::stoll(e); } catch (...) { continue; }
        if (!decodeToken(k, en.key) || !decodeToken(v, en.value)) continue;
        out.push_back(std::move(en));
    }
    return out;
}

void SnapshotManager::remove() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

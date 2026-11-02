#pragma once
#include "common.h"
#include <string>
#include <vector>

// Full-state dump. Written to a temp file then atomically renamed, so a crash never leaves a half-written snapshot.
class SnapshotManager {
public:
    explicit SnapshotManager(const std::string& path) : path_(path) {}
    bool save(const std::vector<Entry>& entries);
    std::vector<Entry> load();
    void remove();

private:
    std::string path_;
};

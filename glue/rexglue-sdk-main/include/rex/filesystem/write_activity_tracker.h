// Tracks host file handles that are open for writing on a save-backed device.
//
// A handle is pending from open-for-write until it is closed or destroyed.
// Synchronous write calls complete before close, so this tracker is the
// host-verifiable completion boundary for save data written through the VFS.
// It is deliberately per-device: only mounts created for saved-game content
// are tracked.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rex::filesystem {

class WriteActivityTracker {
 public:
  struct Snapshot {
    uint32_t open_handles = 0;
    // Sorted, unique host paths with at least one open write handle.
    std::vector<std::string> paths;

    bool idle() const { return open_handles == 0; }
  };

  void Begin(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    ++open_by_path_[path.string()];
  }

  // Returns false if no matching handle was tracked (a caller bug).
  bool End(const std::filesystem::path& path) {
    std::lock_guard lock(mutex_);
    auto it = open_by_path_.find(path.string());
    if (it == open_by_path_.end()) {
      return false;
    }
    if (--it->second == 0) {
      open_by_path_.erase(it);
    }
    return true;
  }

  Snapshot SnapshotState() const {
    std::lock_guard lock(mutex_);
    Snapshot snapshot;
    snapshot.paths.reserve(open_by_path_.size());
    for (const auto& [path, count] : open_by_path_) {
      snapshot.open_handles += count;
      snapshot.paths.push_back(path);
    }
    return snapshot;
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, uint32_t> open_by_path_;
};

}  // namespace rex::filesystem

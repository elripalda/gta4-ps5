#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gta4ps5 {

struct ModFiles {
  std::vector<std::filesystem::path> files;
  std::vector<std::string> errors;
};

// Scan mods/game, returning paths relative to it. Missing folders are empty.
// Symlinks, hidden entries, native binaries and install metadata are excluded.
// All files in a case-insensitive collision are excluded.
ModFiles ScanModFiles(const std::filesystem::path& root);

}  // namespace gta4ps5

// GTA IV PS5 - read-only file replacements, mounted before the game starts.
#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace gta4ps5 {

void MountMods(rex::Runtime& runtime, const std::filesystem::path& app);

}  // namespace gta4ps5

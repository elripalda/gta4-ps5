// GTA IV PS5 - On-console PC texture mod converter (see texture_mod_import.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gta4ps5 {

// Default maximum texture dimension on PS5 (larger mip-mapped textures losslessly
// drop top mip levels without recompression).
inline constexpr uint32_t kDefaultTextureModMaxSize = 2048;

// <app>/mods: where PC texture mods (.oiv, .zip, .wtd, .img, .rpf, .dds, or folders) go.
std::filesystem::path TextureModImportFolder(const std::filesystem::path& app);

// <app>/textures: where converted <xxh64>.dds packs are read by NativeTexturePack.
std::filesystem::path TexturePacksFolder(const std::filesystem::path& app);

// Number of pending mod items (archives, files, or folders excluding "done" and READMEs).
int PendingTextureModImports(const std::filesystem::path& app);

struct TextureModProgress {
  std::string phase;
  std::string detail;
  int done = 0;
  int total = 0;
};

struct TextureModImportResult {
  int mods_processed = 0;
  int textures_converted = 0;
  int textures_copied_dds = 0;
  int textures_skipped = 0;
  bool cancelled = false;
  std::vector<std::string> mod_names;
  std::vector<std::string> errors;
};

using TextureModProgressFn = std::function<void(const TextureModProgress&)>;

// Indexes the installed Xbox 360 textures (caching to <app>/textures/.xbox_index.bin),
// converts every pending mod in <app>/mods into <app>/textures/<mod_name>/<xxh64>.dds
// capped to `max_size`, and moves imported mod items to <app>/mods/done/.
TextureModImportResult ImportTextureMods(
    const std::filesystem::path& app,
    const std::filesystem::path& liberty_root,
    uint32_t max_size = kDefaultTextureModMaxSize,
    const TextureModProgressFn& progress = {},
    const std::atomic<bool>* cancel = nullptr);

}  // namespace gta4ps5

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/ui/vulkan/device.h>

namespace rex::graphics::gta4_native {

struct TexturePackMipLevel {
  uint32_t level = 0;
  uint32_t width = 1;
  uint32_t height = 1;
  uint32_t buffer_row_length = 0;
  uint32_t buffer_image_height = 0;
  size_t payload_offset = 0;
  size_t payload_size = 0;
};

struct LoadedCustomTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mip_count = 0;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  std::vector<uint8_t> payload;
  std::vector<TexturePackMipLevel> mip_levels;
};

// Loads custom <xxh64>.dds texture packs and optionally dumps captured Xbox 360
// textures for authoring packs. All replacement textures upload straight into
// host Vulkan images, bypassing Xbox 360 physical memory limits.
class NativeTexturePack {
 public:
  NativeTexturePack() = default;
  ~NativeTexturePack();

  NativeTexturePack(const NativeTexturePack&) = delete;
  NativeTexturePack& operator=(const NativeTexturePack&) = delete;

  void Initialize(const ui::vulkan::VulkanDevice* vulkan_device, bool packs_enabled,
                  const std::string& packs_dir_override, bool dump_enabled,
                  const std::string& dump_dir_override);
  void Shutdown();

  bool NeedsCanonicalHash() const noexcept {
    return (packs_enabled_ && !index_.empty()) || dump_enabled_.load(std::memory_order_relaxed);
  }
  bool PacksEnabled() const noexcept { return packs_enabled_ && !index_.empty(); }
  bool DumpEnabled() const noexcept { return dump_enabled_.load(std::memory_order_relaxed); }
  uint64_t Generation() const noexcept { return generation_.load(std::memory_order_relaxed); }
  bool HasReplacement(uint64_t hash) const noexcept {
    return packs_enabled_ && index_.find(hash) != index_.end();
  }

  static uint64_t ComputeCanonicalHash(uint32_t width, uint32_t height, uint32_t base_format,
                                       std::span<const uint8_t> mip0_linear);

  static std::filesystem::path ResolveDefaultPacksDir();
  static std::filesystem::path ResolveDefaultDumpDir();

  bool TryLoadReplacement(uint64_t hash, LoadedCustomTexture& out) const;

  void EnqueueDump(uint64_t hash, uint32_t width, uint32_t height, VkFormat vk_format,
                   std::vector<TexturePackMipLevel> mips, std::vector<uint8_t> payload);

 private:
  struct PendingDump {
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    std::vector<TexturePackMipLevel> mips;
    std::vector<uint8_t> payload;
  };

  bool IsFormatSupported(VkFormat format) const noexcept;
  void ScanDirectory(const std::filesystem::path& root);
  void DumpWorkerMain();

  bool packs_enabled_ = false;
  std::atomic<bool> dump_enabled_{false};
  std::atomic<uint64_t> generation_{0};
  std::filesystem::path packs_dir_;
  std::filesystem::path dump_dir_;
  uint32_t max_image_dimension_ = 4096;
  std::unordered_map<uint64_t, std::filesystem::path> index_;
  std::unordered_map<uint32_t, bool> format_support_;

  std::mutex dump_mutex_;
  std::condition_variable dump_cv_;
  bool dump_stop_ = false;
  std::unordered_set<uint64_t> dumped_hashes_;
  std::queue<PendingDump> dump_queue_;
  std::thread dump_worker_;
};

}  // namespace rex::graphics::gta4_native

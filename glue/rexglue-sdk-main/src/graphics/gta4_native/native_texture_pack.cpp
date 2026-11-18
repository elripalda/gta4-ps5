#include "native_texture_pack.h"

#include <rex/graphics/gta4_native/texture_pack_hash.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>

namespace rex::graphics::gta4_native {
namespace {

constexpr uint32_t kDdsMagic = 0x20534444u;        // "DDS "
constexpr uint32_t kDdsFourCcDx10 = 0x30315844u;   // "DX10"
constexpr uint32_t kDdsFourCcDxt1 = 0x31545844u;   // "DXT1"
constexpr uint32_t kDdsFourCcDxt3 = 0x33545844u;   // "DXT3"
constexpr uint32_t kDdsFourCcDxt5 = 0x35545844u;   // "DXT5"
constexpr uint32_t kDdsFourCcAti1 = 0x31495441u;   // "ATI1"
constexpr uint32_t kDdsFourCcBc4u = 0x55344342u;   // "BC4U"
constexpr uint32_t kDdsFourCcAti2 = 0x32495441u;   // "ATI2"
constexpr uint32_t kDdsFourCcBc5u = 0x55354342u;   // "BC5U"

constexpr uint32_t kDdpfAlphaPixels = 0x00000001u;
constexpr uint32_t kDdpfAlpha = 0x00000002u;
constexpr uint32_t kDdpfFourCc = 0x00000004u;
constexpr uint32_t kDdpfRgb = 0x00000040u;
constexpr uint32_t kDdpfLuminance = 0x00020000u;

constexpr uint32_t kDdsCaps2Cubemap = 0x00000200u;
constexpr uint32_t kDdsCaps2Volume = 0x00200000u;

constexpr uint32_t kDxgiFormatR8G8B8A8Unorm = 28u;
constexpr uint32_t kDxgiFormatR8G8B8A8UnormSrgb = 29u;
constexpr uint32_t kDxgiFormatR8G8Unorm = 49u;
constexpr uint32_t kDxgiFormatR8Unorm = 61u;
constexpr uint32_t kDxgiFormatBc1Unorm = 71u;
constexpr uint32_t kDxgiFormatBc1UnormSrgb = 72u;
constexpr uint32_t kDxgiFormatBc2Unorm = 74u;
constexpr uint32_t kDxgiFormatBc2UnormSrgb = 75u;
constexpr uint32_t kDxgiFormatBc3Unorm = 77u;
constexpr uint32_t kDxgiFormatBc3UnormSrgb = 78u;
constexpr uint32_t kDxgiFormatBc4Unorm = 80u;
constexpr uint32_t kDxgiFormatBc5Unorm = 83u;
constexpr uint32_t kDxgiFormatB8G8R8A8Unorm = 87u;
constexpr uint32_t kDxgiFormatB8G8R8X8Unorm = 88u;
constexpr uint32_t kDxgiFormatB8G8R8A8UnormSrgb = 91u;
constexpr uint32_t kDxgiFormatBc7Unorm = 98u;
constexpr uint32_t kDxgiFormatBc7UnormSrgb = 99u;

constexpr size_t kMaxReplacementPayloadBytes = 0x4000000u;  // 64 MiB
constexpr size_t kMaxDumpQueueSize = 256u;

struct DdsPixelFormat {
  uint32_t size = 32;
  uint32_t flags = 0;
  uint32_t four_cc = 0;
  uint32_t rgb_bit_count = 0;
  uint32_t r_mask = 0;
  uint32_t g_mask = 0;
  uint32_t b_mask = 0;
  uint32_t a_mask = 0;
};

struct DdsHeader {
  uint32_t size = 124;
  uint32_t flags = 0;
  uint32_t height = 0;
  uint32_t width = 0;
  uint32_t pitch_or_linear_size = 0;
  uint32_t depth = 0;
  uint32_t mip_map_count = 0;
  uint32_t reserved1[11]{};
  DdsPixelFormat ddspf{};
  uint32_t caps = 0;
  uint32_t caps2 = 0;
  uint32_t caps3 = 0;
  uint32_t caps4 = 0;
  uint32_t reserved2 = 0;
};

struct DdsHeaderDx10 {
  uint32_t dxgi_format = 0;
  uint32_t resource_dimension = 3;  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
  uint32_t misc_flag = 0;
  uint32_t array_size = 1;
  uint32_t misc_flags2 = 0;
};

static_assert(sizeof(DdsPixelFormat) == 32);
static_assert(sizeof(DdsHeader) == 124);
static_assert(sizeof(DdsHeaderDx10) == 20);

struct FormatLayout {
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  uint32_t block_extent = 1;
  uint32_t bytes_per_block = 0;
  bool expand_bgr24_to_rgba = false;
  bool force_opaque_alpha = false;
};

std::optional<uint64_t> ParseHexStem16(std::string_view stem) {
  if (stem.size() != 16) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (unsigned char c : stem) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= uint64_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= uint64_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= uint64_t(c - 'A' + 10);
    } else {
      return std::nullopt;
    }
  }
  return value;
}

bool HasDdsExtension(const std::filesystem::path& path) {
  const std::string ext = path.extension().string();
  return ext.size() == 4 && ext[0] == '.' &&
         std::tolower(static_cast<unsigned char>(ext[1])) == 'd' &&
         std::tolower(static_cast<unsigned char>(ext[2])) == 'd' &&
         std::tolower(static_cast<unsigned char>(ext[3])) == 's';
}

FormatLayout ResolveDdsFormat(const DdsHeader& header, const DdsHeaderDx10* dx10) {
  const DdsPixelFormat& pf = header.ddspf;
  if (dx10) {
    switch (dx10->dxgi_format) {
      case kDxgiFormatBc1Unorm:
      case kDxgiFormatBc1UnormSrgb:
        return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, false, false};
      case kDxgiFormatBc2Unorm:
      case kDxgiFormatBc2UnormSrgb:
        return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, false, false};
      case kDxgiFormatBc3Unorm:
      case kDxgiFormatBc3UnormSrgb:
        return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, false, false};
      case kDxgiFormatBc4Unorm:
        return {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, false, false};
      case kDxgiFormatBc5Unorm:
        return {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, false, false};
      case kDxgiFormatBc7Unorm:
      case kDxgiFormatBc7UnormSrgb:
        return {VK_FORMAT_BC7_UNORM_BLOCK, 4, 16, false, false};
      case kDxgiFormatR8G8B8A8Unorm:
      case kDxgiFormatR8G8B8A8UnormSrgb:
        return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false, false};
      case kDxgiFormatB8G8R8A8Unorm:
      case kDxgiFormatB8G8R8A8UnormSrgb:
        return {VK_FORMAT_B8G8R8A8_UNORM, 1, 4, false, false};
      case kDxgiFormatB8G8R8X8Unorm:
        return {VK_FORMAT_B8G8R8A8_UNORM, 1, 4, false, true};
      case kDxgiFormatR8G8Unorm:
        return {VK_FORMAT_R8G8_UNORM, 1, 2, false, false};
      case kDxgiFormatR8Unorm:
        return {VK_FORMAT_R8_UNORM, 1, 1, false, false};
      default:
        return {};
    }
  }

  if (pf.flags & kDdpfFourCc) {
    switch (pf.four_cc) {
      case kDdsFourCcDxt1:
        return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, false, false};
      case kDdsFourCcDxt3:
        return {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, false, false};
      case kDdsFourCcDxt5:
        return {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, false, false};
      case kDdsFourCcAti1:
      case kDdsFourCcBc4u:
        return {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, false, false};
      case kDdsFourCcAti2:
      case kDdsFourCcBc5u:
        return {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, false, false};
      default:
        return {};
    }
  }

  if (pf.flags & kDdpfRgb) {
    if (pf.rgb_bit_count == 32) {
      const bool has_alpha = (pf.flags & kDdpfAlphaPixels) != 0 && pf.a_mask == 0xFF000000u;
      if (pf.r_mask == 0x00FF0000u && pf.g_mask == 0x0000FF00u && pf.b_mask == 0x000000FFu) {
        return {VK_FORMAT_B8G8R8A8_UNORM, 1, 4, false, !has_alpha};
      }
      if (pf.r_mask == 0x000000FFu && pf.g_mask == 0x0000FF00u && pf.b_mask == 0x00FF0000u) {
        return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false, !has_alpha};
      }
    } else if (pf.rgb_bit_count == 24 && pf.r_mask == 0x00FF0000u &&
               pf.g_mask == 0x0000FF00u && pf.b_mask == 0x000000FFu) {
      return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, true, false};
    }
  }

  if (((pf.flags & kDdpfLuminance) || (pf.flags & kDdpfAlpha)) && pf.rgb_bit_count == 8) {
    return {VK_FORMAT_R8_UNORM, 1, 1, false, false};
  }

  return {};
}

uint32_t DxgiFormatForDump(VkFormat vk_format) {
  switch (vk_format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      return kDxgiFormatBc1Unorm;
    case VK_FORMAT_BC2_UNORM_BLOCK:
      return kDxgiFormatBc2Unorm;
    case VK_FORMAT_BC3_UNORM_BLOCK:
      return kDxgiFormatBc3Unorm;
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return kDxgiFormatBc4Unorm;
    case VK_FORMAT_BC5_UNORM_BLOCK:
      return kDxgiFormatBc5Unorm;
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return kDxgiFormatBc7Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM:
      return kDxgiFormatR8G8B8A8Unorm;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return kDxgiFormatB8G8R8A8Unorm;
    case VK_FORMAT_R8G8_UNORM:
      return kDxgiFormatR8G8Unorm;
    case VK_FORMAT_R8_UNORM:
      return kDxgiFormatR8Unorm;
    default:
      return 0;
  }
}

uint32_t BlockBytesForFormat(VkFormat vk_format) {
  switch (vk_format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
      return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return 16;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
      return 4;
    case VK_FORMAT_R8G8_UNORM:
      return 2;
    case VK_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

uint32_t BlockExtentForFormat(VkFormat vk_format) {
  switch (vk_format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
      return 4;
    default:
      return 1;
  }
}

bool WriteDumpDdsFile(const std::filesystem::path& path, uint32_t width, uint32_t height,
                      VkFormat vk_format, const std::vector<TexturePackMipLevel>& mips,
                      const std::vector<uint8_t>& payload) {
  const uint32_t dxgi_format = DxgiFormatForDump(vk_format);
  if (!dxgi_format || mips.empty() || !width || !height) {
    return false;
  }

  const uint32_t block_extent = BlockExtentForFormat(vk_format);
  const uint32_t block_bytes = BlockBytesForFormat(vk_format);
  const uint32_t blocks_w0 = std::max(1u, (width + block_extent - 1) / block_extent);
  const uint32_t blocks_h0 = std::max(1u, (height + block_extent - 1) / block_extent);

  DdsHeader header{};
  header.size = 124;
  header.flags = 0x1u | 0x2u | 0x4u | 0x1000u;  // CAPS | HEIGHT | WIDTH | PIXELFORMAT
  if (mips.size() > 1) {
    header.flags |= 0x20000u;  // MIPMAPCOUNT
  }
  if (block_extent > 1) {
    header.flags |= 0x80000u;  // LINEARSIZE
    header.pitch_or_linear_size = blocks_w0 * blocks_h0 * block_bytes;
  } else {
    header.flags |= 0x8u;  // PITCH
    header.pitch_or_linear_size = width * block_bytes;
  }
  header.height = height;
  header.width = width;
  header.mip_map_count = static_cast<uint32_t>(mips.size());
  header.caps = 0x1000u;  // TEXTURE
  if (mips.size() > 1) {
    header.caps |= 0x400008u;  // COMPLEX | MIPMAP
  }
  header.ddspf.size = 32;
  header.ddspf.flags = kDdpfFourCc;
  header.ddspf.four_cc = kDdsFourCcDx10;

  DdsHeaderDx10 dx10{};
  dx10.dxgi_format = dxgi_format;
  dx10.resource_dimension = 3;
  dx10.array_size = 1;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(&kDdsMagic), sizeof(kDdsMagic));
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));

  for (const auto& mip : mips) {
    if (mip.payload_offset + mip.payload_size > payload.size()) {
      return false;
    }
    out.write(reinterpret_cast<const char*>(payload.data() + mip.payload_offset),
              static_cast<std::streamsize>(mip.payload_size));
  }
  return out.good();
}

}  // namespace

NativeTexturePack::~NativeTexturePack() { Shutdown(); }

std::filesystem::path NativeTexturePack::ResolveDefaultPacksDir() {
#if REX_PLATFORM_PS5
  return std::filesystem::path("/app0/textures");
#else
  return rex::filesystem::GetUserFolder() / "LibertyRecomp" / "textures";
#endif
}

std::filesystem::path NativeTexturePack::ResolveDefaultDumpDir() {
#if REX_PLATFORM_PS5
  return std::filesystem::path("/app0/textures_dump");
#else
  return rex::filesystem::GetUserFolder() / "LibertyRecomp" / "textures_dump";
#endif
}

uint64_t NativeTexturePack::ComputeCanonicalHash(uint32_t width, uint32_t height,
                                                 uint32_t base_format,
                                                 std::span<const uint8_t> mip0_linear) {
  return ComputeCanonicalTextureHash(width, height, base_format, mip0_linear);
}

void NativeTexturePack::Initialize(const ui::vulkan::VulkanDevice* vulkan_device, bool packs_enabled,
                                   const std::string& packs_dir_override, bool dump_enabled,
                                   const std::string& dump_dir_override) {
  Shutdown();

  packs_enabled_ = packs_enabled;
  dump_enabled_.store(dump_enabled, std::memory_order_relaxed);
  packs_dir_ = packs_dir_override.empty() ? ResolveDefaultPacksDir()
                                          : std::filesystem::path(packs_dir_override);
  dump_dir_ = dump_dir_override.empty() ? ResolveDefaultDumpDir()
                                        : std::filesystem::path(dump_dir_override);

  format_support_.clear();
  if (vulkan_device) {
    max_image_dimension_ = vulkan_device->properties().maxImageDimension2D;
    constexpr std::array<VkFormat, 10> kCandidateFormats = {
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK,      VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK,
        VK_FORMAT_R8G8B8A8_UNORM,       VK_FORMAT_B8G8R8A8_UNORM,  VK_FORMAT_R8G8_UNORM,
        VK_FORMAT_R8_UNORM,
    };
    constexpr VkFormatFeatureFlags kRequired =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (VkFormat fmt : kCandidateFormats) {
      VkFormatProperties props{};
      vulkan_device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          vulkan_device->physical_device(), fmt, &props);
      format_support_[uint32_t(fmt)] = (props.optimalTilingFeatures & kRequired) == kRequired;
    }
  }

  if (packs_enabled_) {
    std::error_code ec;
    std::filesystem::create_directories(packs_dir_, ec);
    ScanDirectory(packs_dir_);
    REXLOG_INFO("gta4-texture-pack: {} replacement(s) indexed in '{}'", index_.size(),
                packs_dir_.string());
  }
  generation_.fetch_add(1, std::memory_order_relaxed);

  if (dump_enabled) {
    std::error_code ec;
    std::filesystem::create_directories(dump_dir_, ec);
    for (const auto& entry : std::filesystem::directory_iterator(dump_dir_, ec)) {
      if (!entry.is_regular_file(ec) || !HasDdsExtension(entry.path())) {
        continue;
      }
      if (const auto hash = ParseHexStem16(entry.path().stem().string())) {
        dumped_hashes_.insert(*hash);
      }
    }
    dump_stop_ = false;
    dump_worker_ = std::thread([this]() { DumpWorkerMain(); });
    REXLOG_INFO("gta4-texture-pack: dumping enabled to '{}' ({} existing files skipped)",
                dump_dir_.string(), dumped_hashes_.size());
  }
}

void NativeTexturePack::Shutdown() {
  {
    std::lock_guard lock(dump_mutex_);
    dump_stop_ = true;
  }
  dump_cv_.notify_all();
  if (dump_worker_.joinable()) {
    dump_worker_.join();
  }
  {
    std::lock_guard lock(dump_mutex_);
    while (!dump_queue_.empty()) {
      dump_queue_.pop();
    }
    dumped_hashes_.clear();
  }
  index_.clear();
  format_support_.clear();
  packs_enabled_ = false;
  dump_enabled_.store(false, std::memory_order_relaxed);
  generation_.fetch_add(1, std::memory_order_relaxed);
}

bool NativeTexturePack::IsFormatSupported(VkFormat format) const noexcept {
  if (format_support_.empty()) {
    return true;
  }
  const auto it = format_support_.find(uint32_t(format));
  return it != format_support_.end() && it->second;
}

void NativeTexturePack::ScanDirectory(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return;
  }
  std::vector<std::filesystem::path> files;
  for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec) || !HasDdsExtension(it->path())) {
      continue;
    }
    const std::string filename = it->path().filename().string();
    if (!filename.empty() && filename[0] == '.') {
      continue;
    }
    files.push_back(it->path());
  }
  // Sort deterministically so later pack subfolders override earlier ones cleanly.
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    if (const auto hash = ParseHexStem16(file.stem().string())) {
      index_[*hash] = file;
    }
  }
}

bool NativeTexturePack::TryLoadReplacement(uint64_t hash, LoadedCustomTexture& out) const {
  if (!packs_enabled_) {
    return false;
  }
  const auto it = index_.find(hash);
  if (it == index_.end()) {
    return false;
  }
  const std::filesystem::path& path = it->second;

  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return false;
  }
  const auto end_pos = stream.tellg();
  if (end_pos < static_cast<std::streamoff>(4 + sizeof(DdsHeader)) ||
      static_cast<uint64_t>(end_pos) > kMaxReplacementPayloadBytes + 4096u) {
    REXLOG_WARN("gta4-texture-pack: skipping '{}' (invalid size)", path.string());
    return false;
  }
  const size_t file_size = static_cast<size_t>(end_pos);
  stream.seekg(0, std::ios::beg);

  uint32_t magic = 0;
  DdsHeader header{};
  if (!stream.read(reinterpret_cast<char*>(&magic), sizeof(magic)) ||
      !stream.read(reinterpret_cast<char*>(&header), sizeof(header))) {
    return false;
  }
  if (magic != kDdsMagic) {
    REXLOG_WARN("gta4-texture-pack: skipping '{}' (not DDS)", path.string());
    return false;
  }
  if (header.size != sizeof(DdsHeader) || header.ddspf.size != sizeof(DdsPixelFormat) ||
      !header.width || !header.height || (header.caps2 & (kDdsCaps2Cubemap | kDdsCaps2Volume))) {
    REXLOG_WARN("gta4-texture-pack: skipping '{}' (unsupported DDS header)", path.string());
    return false;
  }
  if (header.width > max_image_dimension_ || header.height > max_image_dimension_) {
    REXLOG_WARN("gta4-texture-pack: skipping '{}' ({}x{} exceeds GPU limit {})", path.string(),
                header.width, header.height, max_image_dimension_);
    return false;
  }

  size_t data_offset = 4 + sizeof(DdsHeader);
  DdsHeaderDx10 dx10{};
  bool has_dx10 = false;
  if ((header.ddspf.flags & kDdpfFourCc) && header.ddspf.four_cc == kDdsFourCcDx10) {
    if (file_size < data_offset + sizeof(DdsHeaderDx10) ||
        !stream.read(reinterpret_cast<char*>(&dx10), sizeof(dx10))) {
      return false;
    }
    data_offset += sizeof(dx10);
    has_dx10 = true;
    if (dx10.array_size > 1 || dx10.resource_dimension != 3) {
      REXLOG_WARN("gta4-texture-pack: skipping '{}' (non-2D DX10 resource)", path.string());
      return false;
    }
  }

  const FormatLayout layout = ResolveDdsFormat(header, has_dx10 ? &dx10 : nullptr);
  if (layout.vk_format == VK_FORMAT_UNDEFINED || !IsFormatSupported(layout.vk_format)) {
    REXLOG_WARN("gta4-texture-pack: skipping '{}' (unsupported DDS pixel format)", path.string());
    return false;
  }

  const uint32_t requested_mips = std::max(1u, header.mip_map_count);
  const uint32_t max_possible_mips =
      uint32_t(std::bit_width(std::max(header.width, header.height)));
  const uint32_t mip_count = std::min(requested_mips, max_possible_mips);

  out = LoadedCustomTexture{};
  out.width = header.width;
  out.height = header.height;
  out.mip_count = mip_count;
  out.vk_format = layout.vk_format;

  const size_t src_bytes_per_block = layout.expand_bgr24_to_rgba ? 3u : layout.bytes_per_block;
  const size_t dst_align = std::max<size_t>(4u, layout.bytes_per_block);

  out.mip_levels.reserve(mip_count);
  size_t src_cursor = data_offset;
  size_t total_dst_bytes = 0;
  for (uint32_t level = 0; level < mip_count; ++level) {
    const uint32_t mip_w = std::max(1u, header.width >> level);
    const uint32_t mip_h = std::max(1u, header.height >> level);
    const uint32_t blocks_w = std::max(1u, (mip_w + layout.block_extent - 1u) / layout.block_extent);
    const uint32_t blocks_h = std::max(1u, (mip_h + layout.block_extent - 1u) / layout.block_extent);
    const size_t src_mip_size = size_t(blocks_w) * blocks_h * src_bytes_per_block;
    const size_t dst_mip_size = size_t(blocks_w) * blocks_h * layout.bytes_per_block;
    if (src_cursor + src_mip_size > file_size) {
      REXLOG_WARN("gta4-texture-pack: skipping '{}' (truncated at mip {})", path.string(), level);
      out = LoadedCustomTexture{};
      return false;
    }
    const size_t dst_offset = rex::round_up(total_dst_bytes, dst_align, false);
    if (dst_offset + dst_mip_size > kMaxReplacementPayloadBytes) {
      REXLOG_WARN("gta4-texture-pack: skipping '{}' (exceeds 64 MiB payload limit)", path.string());
      out = LoadedCustomTexture{};
      return false;
    }
    TexturePackMipLevel mip_info{};
    mip_info.level = level;
    mip_info.width = mip_w;
    mip_info.height = mip_h;
    mip_info.buffer_row_length = blocks_w * layout.block_extent;
    mip_info.buffer_image_height = blocks_h * layout.block_extent;
    mip_info.payload_offset = dst_offset;
    mip_info.payload_size = dst_mip_size;
    out.mip_levels.push_back(mip_info);
    src_cursor += src_mip_size;
    total_dst_bytes = dst_offset + dst_mip_size;
  }

  out.payload.resize(total_dst_bytes);
  std::vector<uint8_t> bgr24_scratch;
  for (const TexturePackMipLevel& mip_info : out.mip_levels) {
    uint8_t* dst_ptr = out.payload.data() + mip_info.payload_offset;
    if (layout.expand_bgr24_to_rgba) {
      const size_t pixel_count = size_t(mip_info.width) * mip_info.height;
      const size_t src_mip_size = pixel_count * 3u;
      bgr24_scratch.resize(src_mip_size);
      if (!stream.read(reinterpret_cast<char*>(bgr24_scratch.data()),
                       static_cast<std::streamsize>(src_mip_size))) {
        out = LoadedCustomTexture{};
        return false;
      }
      for (size_t p = 0; p < pixel_count; ++p) {
        dst_ptr[p * 4 + 0] = bgr24_scratch[p * 3 + 2];
        dst_ptr[p * 4 + 1] = bgr24_scratch[p * 3 + 1];
        dst_ptr[p * 4 + 2] = bgr24_scratch[p * 3 + 0];
        dst_ptr[p * 4 + 3] = 0xFFu;
      }
    } else {
      if (!stream.read(reinterpret_cast<char*>(dst_ptr),
                       static_cast<std::streamsize>(mip_info.payload_size))) {
        out = LoadedCustomTexture{};
        return false;
      }
      if (layout.force_opaque_alpha && layout.bytes_per_block == 4) {
        const size_t pixel_count = size_t(mip_info.width) * mip_info.height;
        for (size_t p = 0; p < pixel_count; ++p) {
          dst_ptr[p * 4 + 3] = 0xFFu;
        }
      }
    }
  }

  static std::atomic<uint64_t> replaced_count{0};
  const uint64_t count = ++replaced_count;
  if (count <= 16) {
    REXLOG_INFO("gta4-texture-pack: replaced {:016x} from '{}' ({}x{}, {} mips)", hash,
                path.filename().string(), out.width, out.height, out.mip_count);
  }
  return true;
}

void NativeTexturePack::EnqueueDump(uint64_t hash, uint32_t width, uint32_t height,
                                    VkFormat vk_format, std::vector<TexturePackMipLevel> mips,
                                    std::vector<uint8_t> payload) {
  if (!dump_enabled_.load(std::memory_order_relaxed) || !DxgiFormatForDump(vk_format) ||
      mips.empty() || payload.empty()) {
    return;
  }
  {
    std::lock_guard lock(dump_mutex_);
    if (dump_stop_ || !dumped_hashes_.insert(hash).second) {
      return;
    }
    if (dump_queue_.size() >= kMaxDumpQueueSize) {
      dumped_hashes_.erase(hash);
      return;
    }
    PendingDump item{};
    item.hash = hash;
    item.width = width;
    item.height = height;
    item.vk_format = vk_format;
    item.mips = std::move(mips);
    item.payload = std::move(payload);
    dump_queue_.push(std::move(item));
  }
  dump_cv_.notify_one();
}

void NativeTexturePack::DumpWorkerMain() {
  for (;;) {
    PendingDump job;
    {
      std::unique_lock lock(dump_mutex_);
      dump_cv_.wait(lock, [this]() { return dump_stop_ || !dump_queue_.empty(); });
      if (dump_stop_ && dump_queue_.empty()) {
        break;
      }
      job = std::move(dump_queue_.front());
      dump_queue_.pop();
    }
    const std::filesystem::path dest = dump_dir_ / fmt::format("{:016x}.dds", job.hash);
    if (!WriteDumpDdsFile(dest, job.width, job.height, job.vk_format, job.mips, job.payload)) {
      REXLOG_WARN("gta4-texture-pack: failed to write dump '{}'", dest.string());
    }
  }
}

}  // namespace rex::graphics::gta4_native

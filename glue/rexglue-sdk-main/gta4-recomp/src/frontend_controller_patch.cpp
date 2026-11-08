#include "frontend_controller_patch.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>

#include <rex/system/lzx.h>

namespace gta4::button_prompts {
namespace {

constexpr std::array<uint8_t, 4> kResourceMagicLe = {0x52, 0x53, 0x43, 0x05};  // "RSC\5"
constexpr std::array<uint8_t, 4> kResourceMagicBe = {0x05, 0x43, 0x53, 0x52};  // "\5CSR"
constexpr uint32_t kTextureDictionaryType = 0x07;
constexpr uint32_t kDxt5Type = 20;
constexpr uint32_t kLzxWindowSize = 0x20000;
constexpr uint32_t kFrameSize = 0x8000;
constexpr uint32_t kControllerDimension = 512;
constexpr uint32_t kControllerBlocks = kControllerDimension / 4;
constexpr uint32_t kControllerPayloadSize =
    kControllerBlocks * kControllerBlocks * 16;  // 262,144 bytes
constexpr size_t kDdsHeaderSize = 128;
constexpr size_t kXtdHeaderSize = 20;
constexpr size_t kLookaheadBytes = 4;
constexpr uint32_t kResourceOffsetMask = 0x0FFFFFFF;

uint16_t ReadU16Be(std::span<const uint8_t> data, size_t offset) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                               static_cast<uint16_t>(data[offset + 1]));
}

uint32_t ReadU32Be(std::span<const uint8_t> data, size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

uint32_t ReadU32Le(std::span<const uint8_t> data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
         (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void WriteU32Be(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value >> 24);
  data[offset + 1] = static_cast<uint8_t>(value >> 16);
  data[offset + 2] = static_cast<uint8_t>(value >> 8);
  data[offset + 3] = static_cast<uint8_t>(value);
}

uint32_t ResourceOffset(uint32_t value) { return value & kResourceOffsetMask; }

struct XtdHeader {
  bool big_endian = false;
  uint32_t cpu_size = 0;
  uint32_t gpu_size = 0;
  uint32_t compressed_size = 0;
};

bool ParseHeader(std::span<const uint8_t> xtd, XtdHeader& header, std::string& error) {
  if (xtd.size() < kXtdHeaderSize) {
    error = "frontend dictionary is shorter than its 20-byte header";
    return false;
  }
  const bool le_magic = std::equal(kResourceMagicLe.begin(), kResourceMagicLe.end(), xtd.begin());
  const bool be_magic = std::equal(kResourceMagicBe.begin(), kResourceMagicBe.end(), xtd.begin());
  if (le_magic) {
    header.big_endian = false;
  } else if (be_magic) {
    header.big_endian = true;
  } else {
    error = "frontend dictionary has an unexpected resource magic";
    return false;
  }
  if ((header.big_endian ? ReadU32Be(xtd, 4) : ReadU32Le(xtd, 4)) != kTextureDictionaryType) {
    error = "frontend dictionary is not a texture dictionary";
    return false;
  }

  const uint32_t flags =
      header.big_endian ? ReadU32Be(xtd, 8) : ReadU32Le(xtd, 8);
  header.cpu_size = (flags & 0x7FF) << (((flags >> 11) & 0xF) + 8);
  header.gpu_size = ((flags >> 15) & 0x7FF) << (((flags >> 26) & 0xF) + 8);
  header.compressed_size = ReadU32Be(xtd, 16);
  if (header.compressed_size != xtd.size() - kXtdHeaderSize) {
    error = "frontend dictionary compressed size does not match its file size";
    return false;
  }
  if (header.cpu_size == 0 || header.gpu_size == 0) {
    error = "frontend dictionary has empty resource segments";
    return false;
  }
  return true;
}

bool DecodeFrames(std::span<const uint8_t> xtd, const XtdHeader& header, std::vector<uint8_t>& image,
                  std::string& error) {
  const uint64_t total = static_cast<uint64_t>(header.cpu_size) + header.gpu_size;
  if (total > std::numeric_limits<size_t>::max()) {
    error = "frontend dictionary is too large to decode";
    return false;
  }
  image.assign(static_cast<size_t>(total), 0);

  auto decoder = rex::lzx::PersistentDecoder::Create(kLzxWindowSize);
  if (!decoder) {
    error = "could not create the LZX decoder";
    return false;
  }

  size_t position = kXtdHeaderSize;
  size_t written = 0;
  while (position < xtd.size()) {
    uint32_t uncompressed = 0;
    uint32_t compressed = 0;
    size_t payload = 0;
    if (xtd[position] == 0xFF) {
      if (position + 5 > xtd.size()) {
        error = "truncated frontend dictionary frame header";
        return false;
      }
      uncompressed = ReadU16Be(xtd, position + 1);
      compressed = ReadU16Be(xtd, position + 3);
      payload = position + 5;
    } else {
      if (position + 2 > xtd.size()) {
        error = "truncated frontend dictionary frame header";
        return false;
      }
      uncompressed = kFrameSize;
      compressed = ReadU16Be(xtd, position);
      payload = position + 2;
    }
    if (compressed == 0 || payload + compressed > xtd.size() || uncompressed == 0) {
      error = "frontend dictionary frame points outside the file";
      return false;
    }
    if (written + uncompressed > image.size()) {
      error = "frontend dictionary frames exceed the resource size";
      return false;
    }

    // The guest wrapper always presents a few readable bytes after the frame;
    // mirror that contract so the decoder never has to invent padding.
    const size_t readable = std::min<size_t>(
        static_cast<size_t>(compressed) + kLookaheadBytes, xtd.size() - payload);
    std::vector<uint8_t> padded;
    std::span<const uint8_t> frame = xtd.subspan(payload, readable);
    if (readable < static_cast<size_t>(compressed) + kLookaheadBytes) {
      padded.assign(frame.begin(), frame.end());
      padded.resize(static_cast<size_t>(compressed) + kLookaheadBytes, 0);
      frame = padded;
    }

    auto result = decoder->DecodeFrame(frame, std::span(image).subspan(written, uncompressed));
    if (!result || result.bytes_written != uncompressed) {
      error = "LZX decoder rejected a frontend dictionary frame";
      return false;
    }
    written += uncompressed;
    position = payload + compressed;
  }

  if (written != image.size()) {
    error = "frontend dictionary decoded size does not match its header";
    return false;
  }
  return true;
}

struct TextureInfo {
  uint32_t gpu_offset = 0;
};

bool FindControllerTexture(std::span<const uint8_t> image, const XtdHeader& header,
                           TextureInfo& info, std::string& error) {
  const std::span<const uint8_t> cpu = image.first(header.cpu_size);
  if (cpu.size() < 28) {
    error = "frontend dictionary CPU segment is too small";
    return false;
  }
  const uint32_t texture_count = ReadU16Be(cpu, 20);
  const uint32_t list_offset = ResourceOffset(ReadU32Be(cpu, 24));
  if (texture_count == 0 || texture_count > 4096 || list_offset >= cpu.size() ||
      static_cast<uint64_t>(list_offset) + static_cast<uint64_t>(texture_count) * 4 >
          cpu.size()) {
    error = "frontend dictionary texture list is invalid";
    return false;
  }

  for (uint32_t index = 0; index < texture_count; ++index) {
    const uint32_t texture_offset = ResourceOffset(ReadU32Be(cpu, list_offset + index * 4));
    if (static_cast<uint64_t>(texture_offset) + 36 > cpu.size()) {
      error = "frontend dictionary texture record is out of range";
      return false;
    }
    const uint32_t name_offset = ResourceOffset(ReadU32Be(cpu, texture_offset + 20));
    const uint32_t d3d_offset = ResourceOffset(ReadU32Be(cpu, texture_offset + 24));
    const uint16_t width = ReadU16Be(cpu, texture_offset + 28);
    const uint16_t height = ReadU16Be(cpu, texture_offset + 30);
    if (name_offset >= cpu.size() || static_cast<uint64_t>(d3d_offset) + 36 > cpu.size() ||
        width != kControllerDimension || height != kControllerDimension) {
      continue;
    }
    const size_t name_available = cpu.size() - name_offset;
    const auto* name_begin = reinterpret_cast<const char*>(cpu.data() + name_offset);
    const auto* name_end = static_cast<const char*>(std::memchr(name_begin, '\0', name_available));
    if (!name_end) {
      error = "frontend dictionary texture name is unterminated";
      return false;
    }
    const std::string_view name(name_begin, static_cast<size_t>(name_end - name_begin));
    if (!name.ends_with("controller.dds")) {
      continue;
    }

    const uint32_t d3d_descriptor = ReadU32Be(cpu, d3d_offset + 32);
    const uint32_t type = d3d_descriptor & 0x3F;
    const uint32_t gpu_offset = d3d_descriptor & 0x00FFFF00;
    if (type != kDxt5Type) {
      error = "frontend controller texture is not DXT5";
      return false;
    }
    if (static_cast<uint64_t>(gpu_offset) + kControllerPayloadSize > header.gpu_size) {
      error = "frontend controller texture lies outside the GPU segment";
      return false;
    }
    info.gpu_offset = gpu_offset;
    return true;
  }

  error = "frontend dictionary does not contain pack:/controller.dds";
  return false;
}

uint32_t TiledOffset(uint32_t x, uint32_t y) {
  constexpr uint32_t texel_pitch = 16;
  constexpr uint32_t log_bpp = (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2));
  const uint32_t aligned_width = (kControllerBlocks + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log_bpp;
  const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15u) +
                          ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4);
  return (((offset & ~511u) << 3) + ((offset & 448u) << 2) + (offset & 63u) +
          ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
         log_bpp;
}

bool SwizzleController(std::span<const uint8_t> dds, std::vector<uint8_t>& tiled,
                       std::string& error) {
  if (dds.size() != kControllerPayloadSize) {
    error = "controller DDS payload has an unexpected size";
    return false;
  }
  std::vector<uint8_t> swapped(dds.begin(), dds.end());
  for (size_t index = 0; index + 1 < swapped.size(); index += 2) {
    std::swap(swapped[index], swapped[index + 1]);
  }

  tiled.assign(kControllerPayloadSize, 0);
  for (uint32_t y = 0; y < kControllerBlocks; ++y) {
    for (uint32_t x = 0; x < kControllerBlocks; ++x) {
      const uint32_t source = (x + y * kControllerBlocks) * 16;
      const uint32_t target = TiledOffset(x, y) * 16;
      if (target + 16 > kControllerPayloadSize || source + 16 > kControllerPayloadSize) {
        error = "controller tiling produced an out-of-range offset";
        return false;
      }
      std::memcpy(tiled.data() + target, swapped.data() + source, 16);
    }
  }
  return true;
}

bool EncodeFrames(std::span<const uint8_t> image, std::vector<uint8_t>& stream,
                  std::string& error) {
  stream.clear();
  stream.reserve(image.size() + (image.size() / kFrameSize + 1) * 16);
  size_t position = 0;
  bool first = true;
  while (position < image.size()) {
    const uint32_t length = static_cast<uint32_t>(
        std::min<size_t>(kFrameSize, image.size() - position));
    const uint32_t header = ((3u << 24) | length) << (first ? 4u : 5u);
    const size_t payload_size = 16 + length;
    if (payload_size > 0xFFFF) {
      error = "LZX frame does not fit the 16-bit frame length";
      return false;
    }
    stream.push_back(0xFF);
    stream.push_back(static_cast<uint8_t>(length >> 8));
    stream.push_back(static_cast<uint8_t>(length));
    stream.push_back(static_cast<uint8_t>(payload_size >> 8));
    stream.push_back(static_cast<uint8_t>(payload_size));
    stream.push_back(static_cast<uint8_t>(header >> 16));
    stream.push_back(static_cast<uint8_t>(header >> 24));
    stream.push_back(static_cast<uint8_t>(header));
    stream.push_back(static_cast<uint8_t>(header >> 8));
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      stream.insert(stream.end(), {1, 0, 0, 0});
    }
    stream.insert(stream.end(), image.begin() + position, image.begin() + position + length);
    position += length;
    first = false;
  }
  return true;
}

}  // namespace

bool LoadControllerDds(const std::filesystem::path& path, std::vector<uint8_t>& payload,
                       std::string& error) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    error = "could not open '" + path.string() + "'";
    return false;
  }
  const auto end = stream.tellg();
  if (end < 0 || static_cast<uint64_t>(end) != kDdsHeaderSize + kControllerPayloadSize) {
    error = "controller DDS has an unexpected size";
    return false;
  }
  std::vector<uint8_t> data(static_cast<size_t>(end));
  stream.seekg(0, std::ios::beg);
  if (!stream.read(reinterpret_cast<char*>(data.data()), data.size())) {
    error = "could not read '" + path.string() + "'";
    return false;
  }
  if (std::memcmp(data.data(), "DDS ", 4) != 0 ||
      std::memcmp(data.data() + 84, "DXT5", 4) != 0) {
    error = "controller asset is not a DXT5 DDS";
    return false;
  }
  const uint32_t height = ReadU32Le(data, 12);
  const uint32_t width = ReadU32Le(data, 16);
  const uint32_t mip_count = ReadU32Le(data, 28);
  if (width != kControllerDimension || height != kControllerDimension || mip_count != 0) {
    error = "controller DDS must be 512x512 with a single mip";
    return false;
  }
  payload.assign(data.begin() + kDdsHeaderSize, data.end());
  return true;
}

bool PatchFrontendController(std::span<const uint8_t> xtd, std::span<const uint8_t> dds,
                             std::vector<uint8_t>& output, std::string& error) {
  XtdHeader header;
  if (!ParseHeader(xtd, header, error)) {
    return false;
  }
  std::vector<uint8_t> image;
  if (!DecodeFrames(xtd, header, image, error)) {
    return false;
  }

  TextureInfo texture;
  if (!FindControllerTexture(image, header, texture, error)) {
    return false;
  }
  std::vector<uint8_t> tiled;
  if (!SwizzleController(dds, tiled, error)) {
    return false;
  }

  const size_t write_offset = static_cast<size_t>(header.cpu_size) + texture.gpu_offset;
  if (write_offset + kControllerPayloadSize > image.size()) {
    error = "controller texture does not fit inside the decoded dictionary";
    return false;
  }
  std::memcpy(image.data() + write_offset, tiled.data(), tiled.size());

  std::vector<uint8_t> stream;
  if (!EncodeFrames(image, stream, error)) {
    return false;
  }
  if (stream.size() > std::numeric_limits<uint32_t>::max()) {
    error = "frontend dictionary compressed stream is too large";
    return false;
  }

  output.assign(xtd.begin(), xtd.begin() + kXtdHeaderSize);
  WriteU32Be(output, 16, static_cast<uint32_t>(stream.size()));
  output.insert(output.end(), stream.begin(), stream.end());

  std::vector<uint8_t> verify_image;
  if (!DecodeFrames(output, header, verify_image, error)) {
    error = "generated frontend dictionary failed to decode: " + error;
    return false;
  }
  if (verify_image != image) {
    error = "generated frontend dictionary does not round-trip";
    return false;
  }
  return true;
}

}  // namespace gta4::button_prompts

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gta4::button_prompts {

// Reads a 512x512, single-mip DXT5 DDS file and returns its 262,144-byte
// block payload. The header is validated so a mis-packaged asset fails loudly
// instead of producing a corrupted texture.
bool LoadControllerDds(const std::filesystem::path& path, std::vector<uint8_t>& payload,
                       std::string& error);

// Replaces pack:/controller.dds inside an Xbox 360 texture dictionary bundle
// (frontend.xtd / frontend_360.xtd) with the supplied 512x512 DXT5 payload.
// The retail dictionary is decompressed with the runtime LZX decoder, the
// controller surface is re-tiled for the Xbox GPU layout, and the whole
// dictionary is re-emitted as an all-raw LZX stream. The first 16 header bytes
// are preserved and dwInSize is rewritten. The result is decoded again and
// compared against the patched image before it is returned.
bool PatchFrontendController(std::span<const uint8_t> xtd, std::span<const uint8_t> dds,
                             std::vector<uint8_t>& output, std::string& error);

}  // namespace gta4::button_prompts

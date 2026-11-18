#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include <xxhash.h>

namespace rex::graphics::gta4_native {

// Computes the canonical 64-bit XXH64 texture key over (width, height, base_format)
// followed by untiled little-endian mip0 bytes. Shared between runtime texture lookup
// (NativeTexturePack) and on-console/offline mod conversion.
inline uint64_t ComputeCanonicalTextureHash(uint32_t width, uint32_t height, uint32_t base_format,
                                            std::span<const uint8_t> mip0_linear) {
  std::array<uint8_t, 12> meta{};
  std::memcpy(meta.data() + 0, &width, 4);
  std::memcpy(meta.data() + 4, &height, 4);
  std::memcpy(meta.data() + 8, &base_format, 4);

  XXH64_state_t* state = XXH64_createState();
  if (!state) {
    return XXH64(mip0_linear.data(), mip0_linear.size(), 0);
  }
  XXH64_reset(state, 0);
  XXH64_update(state, meta.data(), meta.size());
  if (!mip0_linear.empty()) {
    XXH64_update(state, mip0_linear.data(), mip0_linear.size());
  }
  const uint64_t hash = XXH64_digest(state);
  XXH64_freeState(state);
  return hash;
}

}  // namespace rex::graphics::gta4_native

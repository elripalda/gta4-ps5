#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace gta4::tool {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest Sha256(std::span<const uint8_t> data);
std::string ToHex(const Sha256Digest& digest);
bool Sha256File(const std::string& path, Sha256Digest& digest, std::string& error);

}  // namespace gta4::tool

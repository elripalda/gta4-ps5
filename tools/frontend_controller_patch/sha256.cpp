#include "sha256.h"

#include <array>
#include <fstream>
#include <vector>

namespace gta4::tool {
namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

uint32_t RotateRight(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

void ProcessBlock(const uint8_t* block, std::array<uint32_t, 8>& state) {
  std::array<uint32_t, 64> words = {};
  for (size_t index = 0; index < 16; ++index) {
    words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                   (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[index * 4 + 3]);
  }
  for (size_t index = 16; index < 64; ++index) {
    const uint32_t s0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^
                        (words[index - 15] >> 3);
    const uint32_t s1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^
                        (words[index - 2] >> 10);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (size_t index = 0; index < 64; ++index) {
    const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + words[index];
    const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

Sha256Digest Sha256(std::span<const uint8_t> data) {
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t offset = 0;
  while (data.size() - offset >= 64) {
    ProcessBlock(data.data() + offset, state);
    offset += 64;
  }

  std::array<uint8_t, 128> tail = {};
  const size_t remaining = data.size() - offset;
  for (size_t index = 0; index < remaining; ++index) {
    tail[index] = data[offset + index];
  }
  tail[remaining] = 0x80;
  const size_t tail_size = remaining < 56 ? 64 : 128;
  const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
  for (size_t index = 0; index < 8; ++index) {
    tail[tail_size - 1 - index] = static_cast<uint8_t>(bit_length >> (index * 8));
  }
  ProcessBlock(tail.data(), state);
  if (tail_size == 128) {
    ProcessBlock(tail.data() + 64, state);
  }

  Sha256Digest digest = {};
  for (size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<uint8_t>(state[index] >> 24);
    digest[index * 4 + 1] = static_cast<uint8_t>(state[index] >> 16);
    digest[index * 4 + 2] = static_cast<uint8_t>(state[index] >> 8);
    digest[index * 4 + 3] = static_cast<uint8_t>(state[index]);
  }
  return digest;
}

std::string ToHex(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(digest.size() * 2);
  for (uint8_t byte : digest) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0F]);
  }
  return text;
}

bool Sha256File(const std::string& path, Sha256Digest& digest, std::string& error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "could not open '" + path + "'";
    return false;
  }
  std::vector<uint8_t> buffer(1 << 20);
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint64_t total = 0;
  std::array<uint8_t, 64> pending = {};
  size_t pending_size = 0;

  while (stream) {
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = stream.gcount();
    if (read <= 0) {
      break;
    }
    total += static_cast<uint64_t>(read);
    size_t offset = 0;
    if (pending_size != 0) {
      while (pending_size < 64 && offset < static_cast<size_t>(read)) {
        pending[pending_size++] = buffer[offset++];
      }
      if (pending_size == 64) {
        ProcessBlock(pending.data(), state);
        pending_size = 0;
      }
    }
    while (static_cast<size_t>(read) - offset >= 64) {
      ProcessBlock(buffer.data() + offset, state);
      offset += 64;
    }
    while (offset < static_cast<size_t>(read)) {
      pending[pending_size++] = buffer[offset++];
    }
  }
  if (stream.bad()) {
    error = "could not read '" + path + "'";
    return false;
  }

  pending[pending_size] = 0x80;
  const size_t tail_size = pending_size < 56 ? 64 : 128;
  std::array<uint8_t, 128> tail = {};
  for (size_t index = 0; index <= pending_size; ++index) {
    tail[index] = pending[index];
  }
  tail[pending_size] = 0x80;
  const uint64_t bit_length = total * 8;
  for (size_t index = 0; index < 8; ++index) {
    tail[tail_size - 1 - index] = static_cast<uint8_t>(bit_length >> (index * 8));
  }
  ProcessBlock(tail.data(), state);
  if (tail_size == 128) {
    ProcessBlock(tail.data() + 64, state);
  }

  for (size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<uint8_t>(state[index] >> 24);
    digest[index * 4 + 1] = static_cast<uint8_t>(state[index] >> 16);
    digest[index * 4 + 2] = static_cast<uint8_t>(state[index] >> 8);
    digest[index * 4 + 3] = static_cast<uint8_t>(state[index]);
  }
  return true;
}

}  // namespace gta4::tool

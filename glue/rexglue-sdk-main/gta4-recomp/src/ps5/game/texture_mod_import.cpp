// GTA IV PS5 - On-console PC texture mod converter (.oiv/.zip/.rpf/.img/.wtd/.dds -> <xxh64>.dds).
#include "texture_mod_import.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
extern "C" {
#include <rijndael-alg-fst.h>
}
#include <xxhash.h>
#include <zlib.h>

#include <rex/graphics/gta4_native/texture_pack_hash.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

namespace gta4ps5 {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRpf2Magic = 0x32465052u;     // "RPF2" LE
constexpr uint32_t kImg3Magic = 0xA94E2A52u;
constexpr uint32_t kRsc5MagicLe = 0x05435352u;   // "RSC\x05" LE
constexpr uint32_t kRsc5MagicBe = 0x05435352u;   // byte order checked explicitly
constexpr uint32_t kXmemLzxMagic = 0x0FF512EFu;
constexpr uint32_t kDdsMagic = 0x20534444u;      // "DDS " LE
constexpr uint32_t kDx10FourCc = 0x30315844u;    // "DX10" LE
constexpr uint32_t kIndexCacheMagic = 0x58444958u;  // "XIDX"
constexpr uint32_t kIndexCacheVersion = 1u;

constexpr size_t kMaxInMemoryEntryBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxRscBytes = 256u * 1024u * 1024u;
constexpr size_t kAesKeySize = 32u;

bool SeekAbs(FILE* fp, uint64_t offset) {
#ifdef _WIN32
  return _fseeki64(fp, static_cast<long long>(offset), SEEK_SET) == 0;
#else
  return fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

uint16_t ReadLE16(std::span<const uint8_t> b, size_t off) {
  return static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8);
}

uint32_t ReadLE32(std::span<const uint8_t> b, size_t off) {
  return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint64_t ReadLE64(std::span<const uint8_t> b, size_t off) {
  return static_cast<uint64_t>(ReadLE32(b, off)) |
         (static_cast<uint64_t>(ReadLE32(b, off + 4)) << 32);
}

uint16_t ReadBE16(std::span<const uint8_t> b, size_t off) {
  return (static_cast<uint16_t>(b[off]) << 8) | static_cast<uint16_t>(b[off + 1]);
}

uint32_t ReadBE32(std::span<const uint8_t> b, size_t off) {
  return (static_cast<uint32_t>(b[off]) << 24) | (static_cast<uint32_t>(b[off + 1]) << 16) |
         (static_cast<uint32_t>(b[off + 2]) << 8) | static_cast<uint32_t>(b[off + 3]);
}

void WriteLE32(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v & 0xFFu);
  dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  const size_t start = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[start + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

bool HasAnyExt(std::string_view name, std::span<const std::string_view> exts) {
  for (std::string_view ext : exts) {
    if (EndsWithNoCase(name, ext)) return true;
  }
  return false;
}

bool IsHex16(std::string_view s) {
  if (s.size() != 16) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

std::string NormalizeTexName(std::string_view raw) {
  std::string s(raw);
  for (char& c : s) {
    if (c == '\\') c = '/';
  }
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  if (s.rfind("pack:/", 0) == 0) s.erase(0, 6);
  const size_t slash = s.rfind('/');
  if (slash != std::string::npos) s.erase(0, slash + 1);
  s = LowerAscii(s);
  for (std::string_view ext : {".dds", ".png", ".tga"}) {
    if (EndsWithNoCase(s, ext)) {
      s.resize(s.size() - ext.size());
      break;
    }
  }
  return s;
}

std::string PathBaseName(std::string_view vpath) {
  const size_t slash = vpath.find_last_of("/\\");
  return slash == std::string_view::npos ? std::string(vpath) : std::string(vpath.substr(slash + 1));
}

std::string PathParentName(std::string_view vpath) {
  const size_t slash = vpath.find_last_of("/\\");
  if (slash == std::string_view::npos || slash == 0) return {};
  return PathBaseName(vpath.substr(0, slash));
}

std::string PathStemLower(std::string_view vpath) {
  std::string base = LowerAscii(PathBaseName(vpath));
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos) base.resize(dot);
  return base;
}

std::string SanitizePackFolderName(const fs::path& item) {
  std::string name = item.filename().string();
  std::error_code ec;
  if (!fs::is_directory(item, ec)) {
    const std::string ext = LowerAscii(item.extension().string());
    if (!ext.empty()) name = item.stem().string();
  }
  for (char& c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|') {
      c = '_';
    }
  }
  while (!name.empty() && name.front() == '.') name.front() = '_';
  if (name.empty()) name = "mod_pack";
  return name;
}

bool IsIgnoredModTopEntry(const fs::directory_entry& ent) {
  const std::string name = ent.path().filename().string();
  if (name.empty() || name.front() == '.') return true;
  const std::string low = LowerAscii(name);
  if (low == "done" || low == "readme.txt" || low == "readme.md") return true;
  std::error_code ec;
  if (ent.is_directory(ec)) return false;
  if (!ent.is_regular_file(ec)) return true;
  static constexpr std::array<std::string_view, 6> kTopExts = {".oiv", ".zip", ".rpf",
                                                                ".img", ".wtd", ".dds"};
  return !HasAnyExt(low, kTopExts);
}

bool IsSkippedXboxPath(std::string_view path_low) {
  if (path_low.find("/audio/") != std::string_view::npos || path_low.rfind("audio/", 0) == 0) {
    return true;
  }
  if (path_low.find("/anim/") != std::string_view::npos || path_low.rfind("anim/", 0) == 0) {
    return true;
  }
  static constexpr std::array<std::string_view, 7> kSkipFiles = {
      "audio.rpf", "navmeshes.img", "paths.img", "carrec.img",
      "script.img", "script_network.img", "navgen_script.img",
  };
  const std::string base = PathBaseName(path_low);
  for (std::string_view skip : kSkipFiles) {
    if (base == skip) return true;
  }
  return false;
}

bool IsCancelled(const std::atomic<bool>* cancel) {
  return cancel && cancel->load(std::memory_order_relaxed);
}

// ------------------------------------------------------------------ AesContext

struct AesContext {
  bool valid = false;
  int rounds = 0;
  std::array<u32, 4 * (MAXNR + 1)> round_keys{};

  bool Load(const fs::path& app, const fs::path& liberty_root) {
    std::vector<fs::path> candidates;
#ifdef GTA4_RPF_AES_KEY_SOURCE
    candidates.emplace_back(GTA4_RPF_AES_KEY_SOURCE);
#endif
    if (!app.empty()) candidates.push_back(app / "Resources" / "aes_key.bin");
    if (!liberty_root.empty()) candidates.push_back(liberty_root / "game" / "aes_key.bin");
    for (const fs::path& p : candidates) {
      std::error_code ec;
      if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != kAesKeySize) continue;
      std::array<uint8_t, kAesKeySize> key{};
      std::ifstream in(p, std::ios::binary);
      if (!in.read(reinterpret_cast<char*>(key.data()), key.size())) continue;
      rounds = rijndaelKeySetupDec(round_keys.data(), key.data(), 256);
      valid = rounds > 0;
      if (valid) return true;
    }
    return false;
  }

  void Decrypt16Passes(std::span<uint8_t> data) const {
    if (!valid) return;
    const size_t aligned = data.size() & ~size_t(15);
    std::array<uint8_t, 16> out{};
    for (int pass = 0; pass < 16; ++pass) {
      for (size_t off = 0; off < aligned; off += 16) {
        rijndaelDecrypt(round_keys.data(), rounds, data.data() + off, out.data());
        std::memcpy(data.data() + off, out.data(), 16);
      }
    }
  }
};

// ------------------------------------------------------------------ ByteSource

struct FileReader {
  FILE* fp = nullptr;
  uint64_t file_size = 0;
  fs::path temp_unlink;

  ~FileReader() {
    if (fp) std::fclose(fp);
    if (!temp_unlink.empty()) {
      std::error_code ec;
      fs::remove(temp_unlink, ec);
    }
  }

  static std::shared_ptr<FileReader> Open(const fs::path& path, bool unlink_on_close = false) {
    std::error_code ec;
    const uint64_t sz = fs::file_size(path, ec);
    if (ec) return nullptr;
    FILE* fp = std::fopen(path.string().c_str(), "rb");
    if (!fp) return nullptr;
    auto reader = std::make_shared<FileReader>();
    reader->fp = fp;
    reader->file_size = sz;
    if (unlink_on_close) reader->temp_unlink = path;
    return reader;
  }

  bool ReadAt(uint64_t off, void* dst, size_t len) const {
    if (len == 0) return true;
    if (off > file_size || len > file_size - off) return false;
    if (!SeekAbs(fp, off)) return false;
    return std::fread(dst, 1, len, fp) == len;
  }
};

class ByteSource {
 public:
  ByteSource() = default;

  static ByteSource FromFile(std::shared_ptr<FileReader> file) {
    ByteSource src;
    src.file_ = std::move(file);
    src.size_ = src.file_ ? src.file_->file_size : 0;
    return src;
  }

  static ByteSource FromMemory(std::vector<uint8_t> data) {
    ByteSource src;
    src.size_ = data.size();
    src.mem_ = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    return src;
  }

  bool Valid() const { return file_ != nullptr || mem_ != nullptr; }
  uint64_t Size() const { return size_; }

  ByteSource SubSlice(uint64_t rel_off, uint64_t len) const {
    if (!Valid() || rel_off > size_ || len > size_ - rel_off) return {};
    ByteSource child;
    child.file_ = file_;
    child.mem_ = mem_;
    child.base_ = base_ + rel_off;
    child.size_ = len;
    return child;
  }

  bool ReadAt(uint64_t rel_off, void* dst, size_t len) const {
    if (len == 0) return true;
    if (!Valid() || rel_off > size_ || len > size_ - rel_off) return false;
    if (mem_) {
      std::memcpy(dst, mem_->data() + base_ + rel_off, len);
      return true;
    }
    return file_->ReadAt(base_ + rel_off, dst, len);
  }

  std::optional<std::vector<uint8_t>> ReadVec(uint64_t rel_off, size_t len) const {
    std::vector<uint8_t> out(len);
    if (!ReadAt(rel_off, out.data(), len)) return std::nullopt;
    return out;
  }

  std::optional<std::vector<uint8_t>> ReadAll(size_t max_bytes = kMaxRscBytes) const {
    if (size_ > max_bytes) return std::nullopt;
    return ReadVec(0, static_cast<size_t>(size_));
  }

 private:
  std::shared_ptr<FileReader> file_;
  std::shared_ptr<const std::vector<uint8_t>> mem_;
  uint64_t base_ = 0;
  uint64_t size_ = 0;
};

// ------------------------------------------------------------------ Zlib / LZX

bool InflateWithWindow(std::span<const uint8_t> input, int window_bits, size_t max_out,
                       std::vector<uint8_t>& out) {
  out.clear();
  z_stream zs{};
  if (inflateInit2(&zs, window_bits) != Z_OK) return false;
  zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
  zs.avail_in = static_cast<uInt>(std::min<size_t>(input.size(), std::numeric_limits<uInt>::max()));

  std::array<uint8_t, 64 * 1024> chunk{};
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = chunk.data();
    zs.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    const size_t have = chunk.size() - zs.avail_out;
    if (out.size() + have > max_out) {
      inflateEnd(&zs);
      return false;
    }
    out.insert(out.end(), chunk.data(), chunk.data() + have);
  }
  inflateEnd(&zs);
  return rc == Z_STREAM_END || (rc == Z_BUF_ERROR && !out.empty());
}

bool InflateZlibOrRaw(std::span<const uint8_t> input, size_t max_out, std::vector<uint8_t>& out) {
  if (InflateWithWindow(input, 15, max_out, out)) return true;
  if (input.size() > 2 && input[0] == 0x78) {
    return InflateWithWindow(input.subspan(2), -15, max_out, out);
  }
  return InflateWithWindow(input, -15, max_out, out);
}

bool StreamDeflateToFile(const ByteSource& src, uint64_t comp_off, uint64_t comp_size,
                         const fs::path& dst_path, const std::atomic<bool>* cancel) {
  FILE* out = std::fopen(dst_path.string().c_str(), "wb");
  if (!out) return false;

  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
    std::fclose(out);
    return false;
  }

  std::vector<uint8_t> in_buf(256 * 1024);
  std::vector<uint8_t> out_buf(256 * 1024);
  uint64_t rem = comp_size;
  uint64_t pos = comp_off;
  int rc = Z_OK;
  bool ok = true;

  while (rc != Z_STREAM_END) {
    if (IsCancelled(cancel)) {
      ok = false;
      break;
    }
    if (zs.avail_in == 0 && rem > 0) {
      const size_t want = static_cast<size_t>(std::min<uint64_t>(in_buf.size(), rem));
      if (!src.ReadAt(pos, in_buf.data(), want)) {
        ok = false;
        break;
      }
      pos += want;
      rem -= want;
      zs.next_in = in_buf.data();
      zs.avail_in = static_cast<uInt>(want);
    }
    zs.next_out = out_buf.data();
    zs.avail_out = static_cast<uInt>(out_buf.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    const size_t have = out_buf.size() - zs.avail_out;
    if (have > 0 && std::fwrite(out_buf.data(), 1, have, out) != have) {
      ok = false;
      break;
    }
    if (rc != Z_OK && rc != Z_STREAM_END) {
      ok = false;
      break;
    }
  }

  inflateEnd(&zs);
  std::fclose(out);
  if (!ok) {
    std::error_code ec;
    fs::remove(dst_path, ec);
  }
  return ok;
}

bool DecompressXmemLzx(std::span<const uint8_t> stream, size_t expected_total,
                       std::vector<uint8_t>& out) {
  struct Chunk {
    size_t offset = 0;
    uint16_t comp = 0;
    uint16_t uncomp = 0;
  };
  std::vector<Chunk> chunks;
  size_t total_chunks = 0;
  size_t p = 0;
  while (p + 2 <= stream.size()) {
    uint16_t u = 0x8000u;
    uint16_t c = 0;
    if (stream[p] == 0xFFu) {
      if (p + 5 > stream.size()) break;
      u = ReadBE16(stream, p + 1);
      c = ReadBE16(stream, p + 3);
      p += 5;
    } else {
      c = ReadBE16(stream, p);
      p += 2;
    }
    if (c == 0 || p + c > stream.size()) break;
    chunks.push_back({p, c, u});
    total_chunks += u;
    p += c;
  }
  if (chunks.empty()) return false;

  const size_t alloc_total = std::max(expected_total, total_chunks);
  if (alloc_total > kMaxRscBytes) return false;

  for (uint32_t wbits : {17u, 15u, 16u, 18u}) {
    auto dec = rex::lzx::PersistentDecoder::Create(1u << wbits);
    if (!dec) continue;
    out.assign(alloc_total, 0);
    size_t write_pos = 0;
    bool ok = true;
    for (const Chunk& ch : chunks) {
      if (write_pos + ch.uncomp > out.size()) out.resize(write_pos + ch.uncomp, 0);
      auto res = dec->DecodeFrame(stream.subspan(ch.offset, ch.comp),
                                  std::span<uint8_t>(out).subspan(write_pos, ch.uncomp));
      if (!res) {
        ok = false;
        break;
      }
      write_pos += res.bytes_written;
    }
    if (ok && write_pos > 0) {
      if (out.size() > write_pos) out.resize(write_pos);
      return true;
    }
  }
  out.clear();
  return false;
}

// ------------------------------------------------------------------ RSC5 / XTD / WTD

std::pair<uint32_t, uint32_t> RscSegmentSizes(uint32_t flags) {
  const uint32_t vsize = (flags & 0x7FFu) << (((flags >> 11) & 0xFu) + 8);
  const uint32_t psize = ((flags >> 15) & 0x7FFu) << (((flags >> 26) & 0xFu) + 8);
  return {vsize, psize};
}

std::optional<size_t> RscPtrToOffset(uint32_t ptr, uint32_t vsize, size_t total_size) {
  if (ptr == 0) return std::nullopt;
  const uint32_t sec = ptr >> 28;
  const size_t rel = static_cast<size_t>(ptr & 0x0FFFFFFFu);
  size_t off = 0;
  if (sec == 5) {
    off = rel;
  } else if (sec == 6) {
    if (rel > std::numeric_limits<size_t>::max() - vsize) return std::nullopt;
    off = static_cast<size_t>(vsize) + rel;
  } else {
    return std::nullopt;
  }
  return off < total_size ? std::optional<size_t>(off) : std::nullopt;
}

std::string ReadCStr(std::span<const uint8_t> raw, size_t off) {
  if (off >= raw.size()) return {};
  const char* begin = reinterpret_cast<const char*>(raw.data() + off);
  const size_t avail = raw.size() - off;
  const char* nul = static_cast<const char*>(std::memchr(begin, '\0', avail));
  return std::string(begin, nul ? static_cast<size_t>(nul - begin) : avail);
}

struct XenosFormatSpec {
  uint32_t base_format = 0;
  uint32_t block_extent = 1;
  uint32_t block_bytes = 0;
  uint32_t block_bytes_log2 = 0;
};

std::optional<XenosFormatSpec> ResolveXenosFormat(uint32_t fmt_raw) {
  uint32_t base = fmt_raw;
  switch (fmt_raw) {
    case 32:
    case 56: base = 6; break;
    case 33: base = 18; break;
    case 34: base = 19; break;
    case 35: base = 20; break;
    default: break;
  }
  switch (base) {
    case 2: return XenosFormatSpec{2, 1, 1, 0};     // k_8
    case 6: return XenosFormatSpec{6, 1, 4, 2};     // k_8_8_8_8
    case 18: return XenosFormatSpec{18, 4, 8, 3};   // k_DXT1
    case 19: return XenosFormatSpec{19, 4, 16, 4};  // k_DXT2_3
    case 20: return XenosFormatSpec{20, 4, 16, 4};  // k_DXT4_5
    case 49: return XenosFormatSpec{49, 4, 16, 4};  // k_DXN (BC5)
    case 59: return XenosFormatSpec{59, 4, 8, 3};   // k_DXT5A (BC4)
    default: return std::nullopt;
  }
}

struct XboxParsedEntry {
  std::string tex_name;
  uint64_t hash = 0;
};

std::vector<XboxParsedEntry> ParseXtdBytes(std::span<const uint8_t> data) {
  std::vector<XboxParsedEntry> out;
  if (data.size() < 20) return out;
  uint32_t flags = 0;
  if (data[0] == 0x05 && data[1] == 'C' && data[2] == 'S' && data[3] == 'R') {
    flags = ReadBE32(data, 8);
  } else if (data[0] == 'R' && data[1] == 'S' && data[2] == 'C' && data[3] == 0x05) {
    flags = ReadLE32(data, 8);
  } else {
    return out;
  }

  const auto [vsize, psize] = RscSegmentSizes(flags);
  const size_t total = static_cast<size_t>(vsize) + static_cast<size_t>(psize);
  if (total == 0 || total > kMaxRscBytes) return out;

  std::vector<uint8_t> raw;
  const uint32_t magic2 = ReadBE32(data, 12);
  if (magic2 == kXmemLzxMagic) {
    const uint32_t csize = ReadBE32(data, 16);
    const size_t avail = data.size() - 20;
    const auto stream = data.subspan(20, std::min<size_t>(csize, avail));
    if (!DecompressXmemLzx(stream, total, raw)) return out;
  } else if (data[12] == 0x78) {
    if (!InflateZlibOrRaw(data.subspan(12), total + 65536, raw)) return out;
  } else if (data.size() > 18 && data[16] == 0x78) {
    if (!InflateZlibOrRaw(data.subspan(16), total + 65536, raw)) return out;
  } else {
    raw.assign(data.begin() + 12, data.end());
  }
  if (raw.size() > total) raw.resize(total);
  if (raw.size() < 28) return out;

  const uint16_t cnt = ReadBE16(raw, 20);
  const uint32_t list_ptr = ReadBE32(raw, 24);
  const auto list_off = RscPtrToOffset(list_ptr, vsize, raw.size());
  if (!list_off || *list_off + static_cast<size_t>(cnt) * 4 > raw.size()) return out;

  std::vector<uint8_t> mip0;
  for (uint16_t i = 0; i < cnt; ++i) {
    const uint32_t tex_ptr = ReadBE32(raw, *list_off + i * 4);
    const auto to = RscPtrToOffset(tex_ptr, vsize, raw.size());
    if (!to || *to + 32 > raw.size()) continue;

    const uint32_t name_ptr = ReadBE32(raw, *to + 20);
    const uint32_t d3d_ptr = ReadBE32(raw, *to + 24);
    const uint16_t w_field = ReadBE16(raw, *to + 28);
    const uint16_t h_field = ReadBE16(raw, *to + 30);

    const auto no = RscPtrToOffset(name_ptr, vsize, raw.size());
    const auto d3d_off = RscPtrToOffset(d3d_ptr, vsize, raw.size());
    if (!no || !d3d_off || *d3d_off + 52 > raw.size()) continue;

    const std::string tex_name = NormalizeTexName(ReadCStr(raw, *no));
    if (tex_name.empty()) continue;

    const uint32_t dw0 = ReadBE32(raw, *d3d_off + 28);
    const uint32_t dw1 = ReadBE32(raw, *d3d_off + 32);
    const uint32_t dw2 = ReadBE32(raw, *d3d_off + 36);
    const uint32_t dw4 = ReadBE32(raw, *d3d_off + 44);
    const uint32_t dw5 = ReadBE32(raw, *d3d_off + 48);

    const auto fmt = ResolveXenosFormat(dw1 & 0x3Fu);
    if (!fmt) continue;
    if (((dw5 >> 9) & 0x3u) > 1u) continue;    // 1D or 2D
    if ((dw1 >> 10) & 0x1u) continue;         // stacked
    if ((dw4 >> 2) & 0xFu) continue;          // mip_min_level != 0

    const uint32_t width = dw2 ? ((dw2 & 0x1FFFu) + 1u) : w_field;
    const uint32_t height = dw2 ? (((dw2 >> 13) & 0x1FFFu) + 1u) : h_field;
    if (!width || !height) continue;

    const bool tiled = ((dw0 >> 31) & 1u) != 0;
    const uint32_t pitch_pixels = ((dw0 >> 22) & 0x1FFu) << 5;
    const auto endian = static_cast<rex::graphics::xenos::Endian>((dw1 >> 6) & 0x3u);
    const uint32_t phys_ptr = (dw1 >> 12) << 12;
    const auto phys_off = RscPtrToOffset(phys_ptr, vsize, raw.size());
    if (!phys_off) continue;

    const uint32_t blocks_w = std::max(1u, (width + fmt->block_extent - 1) / fmt->block_extent);
    const uint32_t blocks_h = std::max(1u, (height + fmt->block_extent - 1) / fmt->block_extent);
    uint32_t pitch_blocks = pitch_pixels ? std::max(1u, pitch_pixels / fmt->block_extent) : blocks_w;
    if (tiled) pitch_blocks = (pitch_blocks + 31u) & ~31u;

    const size_t mip0_bytes = static_cast<size_t>(blocks_w) * blocks_h * fmt->block_bytes;
    if (mip0_bytes > 0x4000000ull) continue;
    mip0.resize(mip0_bytes);
    const size_t avail_phys = raw.size() - *phys_off;
    const uint8_t* src_base = raw.data() + *phys_off;

    bool valid = true;
    for (uint32_t by = 0; by < blocks_h && valid; ++by) {
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        const int32_t src_off =
            tiled ? rex::graphics::texture_util::GetTiledOffset2D(
                        static_cast<int32_t>(bx), static_cast<int32_t>(by), pitch_blocks,
                        fmt->block_bytes_log2)
                  : static_cast<int32_t>((static_cast<uint64_t>(by) * pitch_blocks + bx) *
                                         fmt->block_bytes);
        if (src_off < 0 || static_cast<size_t>(src_off) + fmt->block_bytes > avail_phys) {
          valid = false;
          break;
        }
        const size_t dst_off =
            (static_cast<size_t>(by) * blocks_w + bx) * fmt->block_bytes;
        if (fmt->block_bytes == 1) {
          mip0[dst_off] = src_base[src_off];
        } else {
          rex::graphics::texture_conversion::CopySwapBlock(endian, mip0.data() + dst_off,
                                                           src_base + src_off, fmt->block_bytes);
        }
      }
    }
    if (!valid) continue;

    const uint64_t hash = rex::graphics::gta4_native::ComputeCanonicalTextureHash(
        width, height, fmt->base_format, mip0);
    out.push_back({tex_name, hash});
  }
  return out;
}

struct PcTextureAsset {
  std::string dict_name;
  std::string tex_name;
  std::vector<uint8_t> dds;
};

std::vector<uint8_t> BuildDdsBytes(uint32_t width, uint32_t height, uint32_t mip_count,
                                   std::span<const uint8_t> payload,
                                   uint32_t fourcc, uint32_t dxgi_format,
                                   uint32_t rgb_bpp, std::array<uint32_t, 4> masks,
                                   bool is_compressed) {
  uint32_t flags = 0x1u | 0x2u | 0x4u | 0x1000u;
  if (mip_count > 1) flags |= 0x20000u;
  uint32_t caps = 0x1000u;
  if (mip_count > 1) caps |= 0x400008u;

  uint32_t pitch_or_linear = 0;
  if (is_compressed) {
    flags |= 0x80000u;
    const uint32_t bpb = (fourcc == 0x31545844u || dxgi_format == 80u) ? 8u : 16u;
    pitch_or_linear =
        std::max(1u, (width + 3u) / 4u) * std::max(1u, (height + 3u) / 4u) * bpb;
  } else {
    flags |= 0x8u;
    pitch_or_linear = width * (rgb_bpp / 8u);
  }

  const bool has_dx10 = dxgi_format != 0;
  const size_t hdr_size = 128u + (has_dx10 ? 20u : 0u);
  std::vector<uint8_t> out(hdr_size + payload.size(), 0);
  WriteLE32(out.data() + 0, kDdsMagic);
  WriteLE32(out.data() + 4, 124u);
  WriteLE32(out.data() + 8, flags);
  WriteLE32(out.data() + 12, height);
  WriteLE32(out.data() + 16, width);
  WriteLE32(out.data() + 20, pitch_or_linear);
  WriteLE32(out.data() + 28, mip_count);
  // DDS_PIXELFORMAT at offset 76
  WriteLE32(out.data() + 76, 32u);
  if (has_dx10) {
    WriteLE32(out.data() + 80, 0x4u);
    WriteLE32(out.data() + 84, kDx10FourCc);
    WriteLE32(out.data() + 128, dxgi_format);
    WriteLE32(out.data() + 132, 3u);
    WriteLE32(out.data() + 140, 1u);
  } else if (fourcc != 0) {
    WriteLE32(out.data() + 80, 0x4u);
    WriteLE32(out.data() + 84, fourcc);
  } else {
    const uint32_t pf_flags =
        (rgb_bpp == 8 && masks[1] == 0) ? 0x20000u : (0x40u | (masks[3] ? 0x1u : 0u));
    WriteLE32(out.data() + 80, pf_flags);
    WriteLE32(out.data() + 88, rgb_bpp);
    WriteLE32(out.data() + 92, masks[0]);
    WriteLE32(out.data() + 96, masks[1]);
    WriteLE32(out.data() + 100, masks[2]);
    WriteLE32(out.data() + 104, masks[3]);
  }
  WriteLE32(out.data() + 108, caps);
  std::memcpy(out.data() + hdr_size, payload.data(), payload.size());
  return out;
}

std::vector<PcTextureAsset> ParseWtdBytes(std::span<const uint8_t> data, const std::string& dict_name) {
  std::vector<PcTextureAsset> out;
  if (data.size() < 20 || data[0] != 'R' || data[1] != 'S' || data[2] != 'C' || data[3] != 0x05) {
    return out;
  }
  const uint32_t flags = ReadLE32(data, 8);
  const auto [vsize, psize] = RscSegmentSizes(flags);
  const size_t total = static_cast<size_t>(vsize) + static_cast<size_t>(psize);
  if (total == 0 || total > kMaxRscBytes) return out;

  std::vector<uint8_t> raw;
  if (data[12] == 0x78) {
    if (!InflateZlibOrRaw(data.subspan(12), total + 65536, raw)) return out;
  } else if (data.size() > 18 && data[16] == 0x78) {
    if (!InflateZlibOrRaw(data.subspan(16), total + 65536, raw)) return out;
  } else {
    raw.assign(data.begin() + 12, data.end());
  }
  if (raw.size() < 28) return out;

  const uint16_t cnt = ReadLE16(raw, 20);
  const uint32_t list_ptr = ReadLE32(raw, 24);
  const auto list_off = RscPtrToOffset(list_ptr, vsize, raw.size());
  if (!list_off || *list_off + static_cast<size_t>(cnt) * 4 > raw.size()) return out;

  for (uint16_t i = 0; i < cnt; ++i) {
    const uint32_t tex_ptr = ReadLE32(raw, *list_off + i * 4);
    const auto to = RscPtrToOffset(tex_ptr, vsize, raw.size());
    if (!to || *to + 64 > raw.size()) continue;

    const uint32_t name_ptr = ReadLE32(raw, *to + 0x14);
    const auto no = RscPtrToOffset(name_ptr, vsize, raw.size());
    if (!no) continue;
    const std::string tex_name = NormalizeTexName(ReadCStr(raw, *no));
    if (tex_name.empty()) continue;

    const uint16_t width = ReadLE16(raw, *to + 0x1C);
    const uint16_t height = ReadLE16(raw, *to + 0x1E);
    const uint32_t fmt_u32 = ReadLE32(raw, *to + 0x20);
    const uint8_t tex_type = raw[*to + 0x26];
    const uint32_t mip_levels = std::max<uint32_t>(1, raw[*to + 0x27]);
    if (!width || !height || tex_type != 0) continue;

    std::optional<size_t> data_off;
    for (size_t rel_cand : {size_t(0x48), size_t(0x44), size_t(0x3C), size_t(0x40)}) {
      if (*to + rel_cand + 4 <= raw.size()) {
        const uint32_t cand = ReadLE32(raw, *to + rel_cand);
        if ((cand >> 28) == 6u) {
          data_off = RscPtrToOffset(cand, vsize, raw.size());
          if (data_off) break;
        }
      }
    }
    if (!data_off) continue;

    uint32_t fourcc = 0;
    uint32_t dxgi_format = 0;
    bool is_comp = true;
    uint32_t bpb = 16;
    uint32_t blk_ext = 4;
    uint32_t rgb_bpp = 32;
    std::array<uint32_t, 4> masks{};

    if (fmt_u32 == 0x31545844u || fmt_u32 == 0x33545844u || fmt_u32 == 0x35545844u) {
      fourcc = fmt_u32;
      bpb = (fmt_u32 == 0x31545844u) ? 8u : 16u;
    } else if (fmt_u32 == 0x31495441u || fmt_u32 == 0x55344342u) {  // ATI1 / BC4U
      dxgi_format = 80u;
      bpb = 8u;
    } else if (fmt_u32 == 0x32495441u || fmt_u32 == 0x55354342u) {  // ATI2 / BC5U
      dxgi_format = 83u;
      bpb = 16u;
    } else if (fmt_u32 == 21u || fmt_u32 == 22u) {
      is_comp = false;
      blk_ext = 1u;
      bpb = 4u;
      rgb_bpp = 32u;
      masks = {0x00FF0000u, 0x0000FF00u, 0x000000FFu, (fmt_u32 == 21u) ? 0xFF000000u : 0u};
    } else if (fmt_u32 == 28u || fmt_u32 == 50u) {
      is_comp = false;
      blk_ext = 1u;
      bpb = 1u;
      rgb_bpp = 8u;
      masks = {0xFFu, 0u, 0u, 0u};
    } else {
      continue;
    }

    size_t total_bytes = 0;
    for (uint32_t lvl = 0; lvl < mip_levels; ++lvl) {
      const uint32_t mw = std::max(1u, static_cast<uint32_t>(width) >> lvl);
      const uint32_t mh = std::max(1u, static_cast<uint32_t>(height) >> lvl);
      const size_t bw = std::max(1u, (mw + blk_ext - 1) / blk_ext);
      const size_t bh = std::max(1u, (mh + blk_ext - 1) / blk_ext);
      total_bytes += bw * bh * bpb;
    }
    if (*data_off + total_bytes > raw.size()) continue;

    out.push_back({
        dict_name,
        tex_name,
        BuildDdsBytes(width, height, mip_levels,
                      std::span<const uint8_t>(raw).subspan(*data_off, total_bytes), fourcc,
                      dxgi_format, rgb_bpp, masks, is_comp),
    });
  }
  return out;
}

void DowncapDdsInPlace(std::vector<uint8_t>& dds, uint32_t max_size) {
  if (max_size == 0 || dds.size() < 128 || ReadLE32(dds, 0) != kDdsMagic) return;
  const uint32_t h = ReadLE32(dds, 12);
  const uint32_t w = ReadLE32(dds, 16);
  const uint32_t mip_cnt = ReadLE32(dds, 28);
  if (mip_cnt <= 1 || (w <= max_size && h <= max_size)) return;

  const uint32_t pf_flags = ReadLE32(dds, 80);
  const uint32_t fourcc = ReadLE32(dds, 84);
  const uint32_t rgb_bits = ReadLE32(dds, 88);
  size_t data_off = 128;
  uint32_t dxgi_fmt = 0;
  bool has_dx10 = false;
  if ((pf_flags & 0x4u) && fourcc == kDx10FourCc) {
    if (dds.size() < 148) return;
    has_dx10 = true;
    dxgi_fmt = ReadLE32(dds, 128);
    data_off = 148;
  }

  uint32_t blk_ext = 1;
  uint32_t bpb = 0;
  if (has_dx10) {
    if (dxgi_fmt == 71 || dxgi_fmt == 72 || dxgi_fmt == 80) {
      blk_ext = 4; bpb = 8;
    } else if (dxgi_fmt == 74 || dxgi_fmt == 75 || dxgi_fmt == 77 || dxgi_fmt == 78 ||
               dxgi_fmt == 83 || dxgi_fmt == 98 || dxgi_fmt == 99) {
      blk_ext = 4; bpb = 16;
    } else if (dxgi_fmt == 28 || dxgi_fmt == 29 || dxgi_fmt == 87 || dxgi_fmt == 88 ||
               dxgi_fmt == 91) {
      blk_ext = 1; bpb = 4;
    } else if (dxgi_fmt == 49) {
      blk_ext = 1; bpb = 2;
    } else if (dxgi_fmt == 61) {
      blk_ext = 1; bpb = 1;
    }
  } else if (pf_flags & 0x4u) {
    if (fourcc == 0x31545844u || fourcc == 0x31495441u || fourcc == 0x55344342u) {
      blk_ext = 4; bpb = 8;
    } else if (fourcc == 0x33545844u || fourcc == 0x35545844u || fourcc == 0x32495441u ||
               fourcc == 0x55354342u) {
      blk_ext = 4; bpb = 16;
    }
  } else if (rgb_bits == 8 || rgb_bits == 24 || rgb_bits == 32) {
    blk_ext = 1;
    bpb = rgb_bits / 8u;
  }
  if (bpb == 0) return;

  size_t cursor = data_off;
  uint32_t skip = 0;
  uint32_t cw = w;
  uint32_t ch = h;
  while (skip < mip_cnt - 1 && (cw > max_size || ch > max_size)) {
    const size_t bw = std::max(1u, (cw + blk_ext - 1) / blk_ext);
    const size_t bh = std::max(1u, (ch + blk_ext - 1) / blk_ext);
    const size_t mip_sz = bw * bh * bpb;
    if (cursor + mip_sz >= dds.size()) return;
    cursor += mip_sz;
    ++skip;
    cw = std::max(1u, cw >> 1);
    ch = std::max(1u, ch >> 1);
  }
  if (skip == 0) return;

  const size_t keep_bytes = dds.size() - cursor;
  std::memmove(dds.data() + data_off, dds.data() + cursor, keep_bytes);
  dds.resize(data_off + keep_bytes);

  WriteLE32(dds.data() + 12, ch);
  WriteLE32(dds.data() + 16, cw);
  const uint32_t bw0 = std::max(1u, (cw + blk_ext - 1) / blk_ext);
  const uint32_t bh0 = std::max(1u, (ch + blk_ext - 1) / blk_ext);
  const uint32_t lin0 = (blk_ext > 1) ? (bw0 * bh0 * bpb) : (cw * bpb);
  WriteLE32(dds.data() + 20, lin0);
  WriteLE32(dds.data() + 28, mip_cnt - skip);
}

// ------------------------------------------------------------------ Streaming Containers

using ContainerFileCb =
    std::function<void(const std::string& vpath, const ByteSource& src)>;

void WalkByteSource(const ByteSource& src, const std::string& vpath, const AesContext& aes,
                    const fs::path& temp_dir, std::span<const std::string_view> wanted_exts,
                    const ContainerFileCb& cb, const std::atomic<bool>* cancel);

void WalkRpf2(const ByteSource& src, const std::string& archive_vpath, const AesContext& aes,
              const fs::path& temp_dir, std::span<const std::string_view> wanted_exts,
              const ContainerFileCb& cb, const std::atomic<bool>* cancel) {
  std::array<uint8_t, 20> hdr{};
  if (src.Size() < 2048 || !src.ReadAt(0, hdr.data(), hdr.size()) ||
      ReadLE32(hdr, 0) != kRpf2Magic) {
    return;
  }
  const uint32_t toc_size = ReadLE32(hdr, 4);
  const uint32_t entry_count = ReadLE32(hdr, 8);
  const bool encrypted = ReadLE32(hdr, 16) != 0;
  if (entry_count == 0 || entry_count > 100000u ||
      static_cast<uint64_t>(entry_count) * 16u > toc_size ||
      2048ull + toc_size > src.Size()) {
    return;
  }
  auto toc_opt = src.ReadVec(2048, toc_size);
  if (!toc_opt) return;
  auto& toc = *toc_opt;
  if (encrypted) {
    if (!aes.valid) return;
    aes.Decrypt16Passes(toc);
  }
  const size_t str_base = static_cast<size_t>(entry_count) * 16u;

  struct DirWalk {
    uint32_t idx;
    std::string prefix;
  };
  std::vector<DirWalk> stack = {{0u, archive_vpath}};
  std::vector<uint8_t> visited(entry_count, 0);
  visited[0] = 1;

  while (!stack.empty() && !IsCancelled(cancel)) {
    const DirWalk cur = std::move(stack.back());
    stack.pop_back();
    const size_t eoff = static_cast<size_t>(cur.idx) * 16u;
    const uint32_t word2 = ReadLE32(toc, eoff + 8);
    const uint32_t word3 = ReadLE32(toc, eoff + 12);
    if ((word2 & 0x80000000u) == 0) continue;
    const uint32_t child_start = word2 & 0x7FFFFFFFu;
    const uint32_t child_count = word3 & 0x0FFFFFFFu;
    if (static_cast<uint64_t>(child_start) + child_count > entry_count) continue;

    for (uint32_t c = 0; c < child_count && !IsCancelled(cancel); ++c) {
      const uint32_t idx = child_start + c;
      const size_t ceoff = static_cast<size_t>(idx) * 16u;
      const uint32_t name_off = ReadLE32(toc, ceoff);
      const std::string name =
          (str_base + name_off < toc.size()) ? ReadCStr(toc, str_base + name_off) : "";
      if (name.empty() || name == "." || name == "..") continue;
      const std::string child_vpath = cur.prefix + "/" + name;
      if (IsSkippedXboxPath(LowerAscii(child_vpath))) continue;

      const uint32_t c_w1 = ReadLE32(toc, ceoff + 4);
      const uint32_t c_w2 = ReadLE32(toc, ceoff + 8);
      const uint32_t c_w3 = ReadLE32(toc, ceoff + 12);
      if ((c_w2 & 0x80000000u) != 0) {
        if (!visited[idx]) {
          visited[idx] = 1;
          stack.push_back({idx, child_vpath});
        }
        continue;
      }
      if (!HasAnyExt(name, wanted_exts)) continue;

      const bool is_res = (c_w3 & 0xC0000000u) == 0xC0000000u;
      const uint64_t offset = is_res ? (c_w2 & 0x7FFFFFFFu) : c_w2;
      const uint32_t arch_size = is_res ? c_w1 : (c_w3 & 0x3FFFFFFFu);
      const bool compressed = !is_res && ((c_w3 & 0x40000000u) != 0);
      if (offset + arch_size > src.Size()) continue;

      if (!compressed) {
        WalkByteSource(src.SubSlice(offset, arch_size), child_vpath, aes, temp_dir, wanted_exts, cb,
                       cancel);
      } else if (arch_size <= kMaxRscBytes) {
        auto raw_opt = src.ReadVec(offset, arch_size);
        if (!raw_opt) continue;
        std::vector<uint8_t> inflated;
        if (!InflateZlibOrRaw(*raw_opt, kMaxRscBytes, inflated)) continue;
        WalkByteSource(ByteSource::FromMemory(std::move(inflated)), child_vpath, aes, temp_dir,
                       wanted_exts, cb, cancel);
      }
    }
  }
}

void WalkImg3(const ByteSource& src, const std::string& archive_vpath, const AesContext& aes,
              const fs::path& temp_dir, std::span<const std::string_view> wanted_exts,
              const ContainerFileCb& cb, const std::atomic<bool>* cancel) {
  std::array<uint8_t, 20> hdr{};
  if (src.Size() < 20 || !src.ReadAt(0, hdr.data(), hdr.size())) return;
  uint32_t magic = ReadLE32(hdr, 0);
  uint32_t version = ReadLE32(hdr, 4);
  uint32_t num_items = ReadLE32(hdr, 8);
  uint32_t toc_size = ReadLE32(hdr, 12);
  bool encrypted = false;
  if (magic != kImg3Magic || version != 3u) {
    if (!aes.valid) return;
    aes.Decrypt16Passes(std::span<uint8_t>(hdr).first(16));
    magic = ReadLE32(hdr, 0);
    version = ReadLE32(hdr, 4);
    num_items = ReadLE32(hdr, 8);
    toc_size = ReadLE32(hdr, 12);
    if (magic != kImg3Magic || version != 3u) return;
    encrypted = true;
  }
  if (num_items > 200000u || toc_size < static_cast<uint64_t>(num_items) * 16u ||
      20ull + toc_size > src.Size()) {
    return;
  }

  auto toc_opt = src.ReadVec(20, toc_size);
  if (!toc_opt) return;
  auto& toc = *toc_opt;
  if (encrypted) aes.Decrypt16Passes(toc);

  size_t str_pos = static_cast<size_t>(num_items) * 16u;
  for (uint32_t i = 0; i < num_items && !IsCancelled(cancel); ++i) {
    const size_t ioff = static_cast<size_t>(i) * 16u;
    const uint32_t sz_or_flags = ReadLE32(toc, ioff);
    const uint32_t blk_off = ReadLE32(toc, ioff + 8);
    const uint16_t used_blks = ReadLE16(toc, ioff + 12);
    const uint16_t flags = ReadLE16(toc, ioff + 14);
    if (str_pos >= toc.size()) break;
    const std::string name = ReadCStr(toc, str_pos);
    str_pos += name.size() + 1;
    if (name.empty() || !HasAnyExt(name, wanted_exts)) continue;

    const uint64_t off = static_cast<uint64_t>(blk_off) * 2048ull;
    const uint64_t alloc = static_cast<uint64_t>(used_blks) * 2048ull;
    const uint64_t pad = flags & 0x7FFu;
    const uint64_t exact = (alloc >= pad && alloc > 0) ? (alloc - pad) : sz_or_flags;
    if (off + exact > src.Size()) continue;

    WalkByteSource(src.SubSlice(off, exact), archive_vpath + "/" + name, aes, temp_dir, wanted_exts,
                   cb, cancel);
  }
}

void WalkZip(const ByteSource& src, const std::string& archive_vpath, const AesContext& aes,
             const fs::path& temp_dir, std::span<const std::string_view> wanted_exts,
             const ContainerFileCb& cb, const std::atomic<bool>* cancel) {
  if (src.Size() < 22) return;
  const size_t tail_len = static_cast<size_t>(std::min<uint64_t>(src.Size(), 65557ull));
  const uint64_t tail_off = src.Size() - tail_len;
  auto tail_opt = src.ReadVec(tail_off, tail_len);
  if (!tail_opt) return;
  const auto& tail = *tail_opt;

  size_t eocd_rel = std::string::npos;
  for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
    if (tail[i] == 0x50 && tail[i + 1] == 0x4B && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
      eocd_rel = i;
      break;
    }
  }
  if (eocd_rel == std::string::npos) return;

  uint64_t total_entries = ReadLE16(tail, eocd_rel + 10);
  uint64_t cd_size = ReadLE32(tail, eocd_rel + 12);
  uint64_t cd_off = ReadLE32(tail, eocd_rel + 16);

  // ZIP64 End of Central Directory support for large mod packages (> 4 GiB).
  if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || total_entries == 0xFFFFu) {
    if (tail_off + eocd_rel >= 20) {
      std::array<uint8_t, 20> loc{};
      if (src.ReadAt(tail_off + eocd_rel - 20, loc.data(), loc.size()) &&
          loc[0] == 0x50 && loc[1] == 0x4B && loc[2] == 0x06 && loc[3] == 0x07) {
        const uint64_t z64_off = ReadLE64(loc, 8);
        std::array<uint8_t, 56> z64{};
        if (src.ReadAt(z64_off, z64.data(), z64.size()) &&
            z64[0] == 0x50 && z64[1] == 0x4B && z64[2] == 0x06 && z64[3] == 0x06) {
          total_entries = ReadLE64(z64, 32);
          cd_size = ReadLE64(z64, 40);
          cd_off = ReadLE64(z64, 48);
        }
      }
    }
  }

  if (cd_size == 0 || cd_size > 64u * 1024u * 1024u || cd_off + cd_size > src.Size()) return;
  auto cd_opt = src.ReadVec(cd_off, static_cast<size_t>(cd_size));
  if (!cd_opt) return;
  const auto& cd = *cd_opt;

  size_t pos = 0;
  for (uint64_t idx = 0; idx < total_entries && pos + 46 <= cd.size() && !IsCancelled(cancel);
       ++idx) {
    if (cd[pos] != 0x50 || cd[pos + 1] != 0x4B || cd[pos + 2] != 0x01 || cd[pos + 3] != 0x02) {
      break;
    }
    const uint16_t method = ReadLE16(cd, pos + 10);
    uint64_t comp_size = ReadLE32(cd, pos + 20);
    uint64_t uncomp_size = ReadLE32(cd, pos + 24);
    const uint16_t fname_len = ReadLE16(cd, pos + 28);
    const uint16_t extra_len = ReadLE16(cd, pos + 30);
    const uint16_t comment_len = ReadLE16(cd, pos + 32);
    uint64_t local_off = ReadLE32(cd, pos + 42);
    const size_t next_pos = pos + 46 + fname_len + extra_len + comment_len;
    if (next_pos > cd.size()) break;

    std::string fname(reinterpret_cast<const char*>(cd.data() + pos + 46), fname_len);
    for (char& c : fname) {
      if (c == '\\') c = '/';
    }

    // Read ZIP64 extra field (header id 0x0001) when 32-bit fields overflow.
    size_t ex_pos = pos + 46 + fname_len;
    const size_t ex_end = ex_pos + extra_len;
    while (ex_pos + 4 <= ex_end) {
      const uint16_t eid = ReadLE16(cd, ex_pos);
      const uint16_t esz = ReadLE16(cd, ex_pos + 2);
      if (ex_pos + 4 + esz > ex_end) break;
      if (eid == 0x0001u) {
        size_t f = ex_pos + 4;
        if (uncomp_size == 0xFFFFFFFFu && f + 8 <= ex_pos + 4 + esz) {
          uncomp_size = ReadLE64(cd, f); f += 8;
        }
        if (comp_size == 0xFFFFFFFFu && f + 8 <= ex_pos + 4 + esz) {
          comp_size = ReadLE64(cd, f); f += 8;
        }
        if (local_off == 0xFFFFFFFFu && f + 8 <= ex_pos + 4 + esz) {
          local_off = ReadLE64(cd, f);
        }
      }
      ex_pos += 4 + esz;
    }
    pos = next_pos;

    if (fname.empty() || fname.back() == '/' || fname.find("..") != std::string::npos) continue;
    if (!HasAnyExt(fname, wanted_exts)) continue;

    std::array<uint8_t, 30> lhdr{};
    if (local_off + 30 > src.Size() || !src.ReadAt(local_off, lhdr.data(), lhdr.size()) ||
        lhdr[0] != 0x50 || lhdr[1] != 0x4B || lhdr[2] != 0x03 || lhdr[3] != 0x04) {
      continue;
    }
    const uint64_t data_off = local_off + 30 + ReadLE16(lhdr, 26) + ReadLE16(lhdr, 28);
    if (data_off + comp_size > src.Size()) continue;

    const std::string entry_vpath = archive_vpath + "/" + fname;
    if (method == 0) {
      WalkByteSource(src.SubSlice(data_off, comp_size), entry_vpath, aes, temp_dir, wanted_exts, cb,
                     cancel);
    } else if (method == 8) {
      if (uncomp_size <= kMaxInMemoryEntryBytes && comp_size <= kMaxInMemoryEntryBytes) {
        auto comp_opt = src.ReadVec(data_off, static_cast<size_t>(comp_size));
        if (!comp_opt) continue;
        std::vector<uint8_t> inflated;
        if (!InflateWithWindow(*comp_opt, -MAX_WBITS, kMaxInMemoryEntryBytes + 1024, inflated)) {
          continue;
        }
        WalkByteSource(ByteSource::FromMemory(std::move(inflated)), entry_vpath, aes, temp_dir,
                       wanted_exts, cb, cancel);
      } else if (!temp_dir.empty()) {
        std::error_code ec;
        fs::create_directories(temp_dir, ec);
        const fs::path tmp_file = temp_dir / ".mod_zip_tmp.bin";
        if (!StreamDeflateToFile(src, data_off, comp_size, tmp_file, cancel)) continue;
        if (auto fr = FileReader::Open(tmp_file, /*unlink_on_close=*/true)) {
          WalkByteSource(ByteSource::FromFile(std::move(fr)), entry_vpath, aes, temp_dir,
                         wanted_exts, cb, cancel);
        }
      }
    }
  }
}

void WalkByteSource(const ByteSource& src, const std::string& vpath, const AesContext& aes,
                    const fs::path& temp_dir, std::span<const std::string_view> wanted_exts,
                    const ContainerFileCb& cb, const std::atomic<bool>* cancel) {
  if (!src.Valid() || IsCancelled(cancel)) return;
  if (EndsWithNoCase(vpath, ".rpf")) {
    WalkRpf2(src, vpath, aes, temp_dir, wanted_exts, cb, cancel);
  } else if (EndsWithNoCase(vpath, ".img")) {
    WalkImg3(src, vpath, aes, temp_dir, wanted_exts, cb, cancel);
  } else if (EndsWithNoCase(vpath, ".zip") || EndsWithNoCase(vpath, ".oiv")) {
    WalkZip(src, vpath, aes, temp_dir, wanted_exts, cb, cancel);
  } else if (HasAnyExt(vpath, wanted_exts)) {
    cb(vpath, src);
  }
}

// ------------------------------------------------------------------ Xbox Index + Cache

struct XboxTextureIndex {
  std::unordered_map<std::string, std::vector<uint64_t>> by_pair;  // "dict\ntex" -> unique hashes
  std::unordered_map<std::string, std::vector<uint64_t>> by_tex;   // "tex" -> unique hashes
};

void AddUnique(std::vector<uint64_t>& vec, uint64_t h) {
  if (std::find(vec.begin(), vec.end(), h) == vec.end()) vec.push_back(h);
}

uint64_t ComputeGameFingerprint(const fs::path& game_dir) {
  static constexpr std::array<std::string_view, 3> kXboxExts = {".rpf", ".img", ".xtd"};
  std::vector<std::pair<std::string, std::pair<uint64_t, int64_t>>> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(game_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    std::string rel = it->path().lexically_relative(game_dir).generic_string();
    if (!HasAnyExt(rel, kXboxExts) || IsSkippedXboxPath(LowerAscii(rel))) continue;
    const uint64_t sz = it->file_size(ec);
    const auto mt = it->last_write_time(ec).time_since_epoch().count();
    files.emplace_back(std::move(rel), std::make_pair(sz, static_cast<int64_t>(mt)));
  }
  std::sort(files.begin(), files.end());

  XXH64_state_t* st = XXH64_createState();
  if (!st) return 0;
  XXH64_reset(st, 0);
  for (const auto& f : files) {
    XXH64_update(st, f.first.data(), f.first.size());
    XXH64_update(st, &f.second.first, sizeof(f.second.first));
    XXH64_update(st, &f.second.second, sizeof(f.second.second));
  }
  const uint64_t fp = XXH64_digest(st);
  XXH64_freeState(st);
  return fp;
}

bool LoadIndexCache(const fs::path& cache_path, uint64_t expected_fp, XboxTextureIndex& idx) {
  std::ifstream in(cache_path, std::ios::binary);
  if (!in) return false;
  struct CacheHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t fingerprint;
    uint32_t pair_count;
    uint32_t tex_count;
  } hdr{};
  if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) || hdr.magic != kIndexCacheMagic ||
      hdr.version != kIndexCacheVersion || hdr.fingerprint != expected_fp) {
    return false;
  }
  auto read_str = [&](std::string& s) -> bool {
    uint16_t len = 0;
    if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
    s.resize(len);
    return len == 0 || static_cast<bool>(in.read(s.data(), len));
  };
  auto read_hashes = [&](std::vector<uint64_t>& vec) -> bool {
    uint16_t count = 0;
    if (!in.read(reinterpret_cast<char*>(&count), sizeof(count))) return false;
    vec.resize(count);
    return count == 0 ||
           static_cast<bool>(in.read(reinterpret_cast<char*>(vec.data()), count * sizeof(uint64_t)));
  };
  idx.by_pair.reserve(hdr.pair_count);
  for (uint32_t i = 0; i < hdr.pair_count; ++i) {
    std::string k;
    std::vector<uint64_t> hs;
    if (!read_str(k) || !read_hashes(hs)) return false;
    idx.by_pair.emplace(std::move(k), std::move(hs));
  }
  idx.by_tex.reserve(hdr.tex_count);
  for (uint32_t i = 0; i < hdr.tex_count; ++i) {
    std::string k;
    std::vector<uint64_t> hs;
    if (!read_str(k) || !read_hashes(hs)) return false;
    idx.by_tex.emplace(std::move(k), std::move(hs));
  }
  return true;
}

void SaveIndexCache(const fs::path& cache_path, uint64_t fp, const XboxTextureIndex& idx) {
  std::error_code ec;
  fs::create_directories(cache_path.parent_path(), ec);
  const fs::path tmp = cache_path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    struct CacheHeader {
      uint32_t magic;
      uint32_t version;
      uint64_t fingerprint;
      uint32_t pair_count;
      uint32_t tex_count;
    } hdr{kIndexCacheMagic, kIndexCacheVersion, fp,
          static_cast<uint32_t>(idx.by_pair.size()),
          static_cast<uint32_t>(idx.by_tex.size())};
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    auto write_map = [&](const std::unordered_map<std::string, std::vector<uint64_t>>& m) {
      for (const auto& [k, hs] : m) {
        const uint16_t klen = static_cast<uint16_t>(std::min<size_t>(k.size(), 65535));
        out.write(reinterpret_cast<const char*>(&klen), sizeof(klen));
        out.write(k.data(), klen);
        const uint16_t count = static_cast<uint16_t>(std::min<size_t>(hs.size(), 65535));
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(hs.data()), count * sizeof(uint64_t));
      }
    };
    write_map(idx.by_pair);
    write_map(idx.by_tex);
    if (!out) return;
  }
  fs::rename(tmp, cache_path, ec);
}

bool BuildXboxIndex(const fs::path& game_dir, const fs::path& cache_path, const AesContext& aes,
                    const fs::path& temp_dir, XboxTextureIndex& idx,
                    const TextureModProgressFn& progress, const std::atomic<bool>* cancel) {
  const uint64_t fp = ComputeGameFingerprint(game_dir);
  if (!cache_path.empty() && LoadIndexCache(cache_path, fp, idx)) {
    REXLOG_INFO("PS5 texture import: loaded cached Xbox 360 index ({} keys)", idx.by_tex.size());
    return true;
  }

  static constexpr std::array<std::string_view, 3> kXboxExts = {".rpf", ".img", ".xtd"};
  std::vector<fs::path> xbox_files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(game_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string rel = it->path().lexically_relative(game_dir).generic_string();
    if (!HasAnyExt(rel, kXboxExts) || IsSkippedXboxPath(LowerAscii(rel))) continue;
    xbox_files.push_back(it->path());
  }
  std::sort(xbox_files.begin(), xbox_files.end());

  int xtd_count = 0;
  for (size_t i = 0; i < xbox_files.size() && !IsCancelled(cancel); ++i) {
    const fs::path& f = xbox_files[i];
    if (progress) {
      progress({fmt::format("Indexing Xbox 360 textures ({}/{})...", i + 1, xbox_files.size()),
                f.filename().string(), static_cast<int>(i), static_cast<int>(xbox_files.size())});
    }
    auto fr = FileReader::Open(f);
    if (!fr) continue;
    const std::string rel = f.lexically_relative(game_dir).generic_string();
    WalkByteSource(ByteSource::FromFile(std::move(fr)), rel, aes, temp_dir, kXboxExts,
                   [&](const std::string& vpath, const ByteSource& xtd_src) {
                     if (!EndsWithNoCase(vpath, ".xtd")) return;
                     auto bytes = xtd_src.ReadAll();
                     if (!bytes) return;
                     ++xtd_count;
                     if (progress && (xtd_count % 25 == 0)) {
                       progress({fmt::format("Indexing Xbox 360 textures (first-time setup)..."),
                                 fmt::format("{} dictionaries · {}", xtd_count, PathBaseName(vpath)),
                                 0, 0});
                     }
                     const std::string dict = PathStemLower(vpath);
                     for (const XboxParsedEntry& ent : ParseXtdBytes(*bytes)) {
                       AddUnique(idx.by_pair[dict + "\n" + ent.tex_name], ent.hash);
                       AddUnique(idx.by_tex[ent.tex_name], ent.hash);
                     }
                   },
                   cancel);
  }
  if (IsCancelled(cancel)) return false;

  REXLOG_INFO("PS5 texture import: indexed {} unique names across {} .xtd dictionaries",
              idx.by_tex.size(), xtd_count);
  if (!cache_path.empty() && !idx.by_tex.empty()) {
    SaveIndexCache(cache_path, fp, idx);
  }
  return true;
}

bool WriteDdsFile(const fs::path& dst, const std::vector<uint8_t>& dds) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
  return static_cast<bool>(out);
}

void ProcessSingleModItem(const fs::path& mod_item, const fs::path& out_pack_dir,
                          const XboxTextureIndex& idx, const AesContext& aes,
                          const fs::path& temp_dir, uint32_t max_size,
                          int mod_idx, int mod_total,
                          const TextureModProgressFn& progress, const std::atomic<bool>* cancel,
                          TextureModImportResult& result) {
  static constexpr std::array<std::string_view, 6> kModExts = {".oiv", ".zip", ".rpf",
                                                                ".img", ".wtd", ".dds"};
  const std::string item_name = mod_item.filename().string();
  int mod_converted = 0;
  int mod_copied_dds = 0;
  int mod_skipped = 0;
  int last_reported = 0;

  auto report_mod = [&](const std::string& detail_file) {
    if (!progress) return;
    last_reported = mod_converted + mod_copied_dds + mod_skipped;
    progress({
        fmt::format("Converting mod {}/{}: {}", mod_idx + 1, mod_total, item_name),
        fmt::format("{} converted · {} skipped ({}px cap){}", mod_converted + mod_copied_dds,
                    mod_skipped, max_size,
                    detail_file.empty() ? "" : " · " + detail_file),
        mod_idx,
        mod_total,
    });
  };
  report_mod("");

  std::vector<fs::path> host_files;
  std::error_code ec;
  if (fs::is_directory(mod_item, ec)) {
    for (fs::recursive_directory_iterator it(mod_item, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (HasAnyExt(it->path().filename().string(), kModExts)) {
        host_files.push_back(it->path());
      }
    }
    std::sort(host_files.begin(), host_files.end());
  } else {
    host_files.push_back(mod_item);
  }

  for (const fs::path& f : host_files) {
    if (IsCancelled(cancel)) break;
    auto fr = FileReader::Open(f);
    if (!fr) {
      result.errors.push_back(item_name + ": failed to open " + f.filename().string());
      continue;
    }
    const std::string rel =
        fs::is_directory(mod_item, ec) ? f.lexically_relative(mod_item).generic_string()
                                       : f.filename().string();
    WalkByteSource(
        ByteSource::FromFile(std::move(fr)), rel, aes, temp_dir, kModExts,
        [&](const std::string& vpath, const ByteSource& leaf) {
          if (IsCancelled(cancel)) return;
          if (EndsWithNoCase(vpath, ".wtd")) {
            auto bytes = leaf.ReadAll();
            if (!bytes) return;
            const std::string dict = PathStemLower(vpath);
            auto assets = ParseWtdBytes(*bytes, dict);
            for (auto& asset : assets) {
              const std::vector<uint64_t>* targets = nullptr;
              auto pit = idx.by_pair.find(asset.dict_name + "\n" + asset.tex_name);
              if (pit != idx.by_pair.end()) {
                targets = &pit->second;
              } else {
                auto tit = idx.by_tex.find(asset.tex_name);
                if (tit != idx.by_tex.end() && tit->second.size() == 1) {
                  targets = &tit->second;
                }
              }
              if (!targets || targets->empty()) {
                ++mod_skipped;
                continue;
              }
              DowncapDdsInPlace(asset.dds, max_size);
              for (uint64_t h : *targets) {
                const fs::path dst = out_pack_dir / fmt::format("{:016x}.dds", h);
                if (WriteDdsFile(dst, asset.dds)) {
                  ++mod_converted;
                } else {
                  result.errors.push_back(fmt::format("{}: write failed ({})", item_name, dst.string()));
                }
              }
            }
            if ((mod_converted + mod_copied_dds + mod_skipped) - last_reported >= 16) {
              report_mod(PathBaseName(vpath));
            }
          } else if (EndsWithNoCase(vpath, ".dds")) {
            auto bytes = leaf.ReadAll();
            if (!bytes) return;
            const std::string stem = PathStemLower(vpath);
            DowncapDdsInPlace(*bytes, max_size);
            if (IsHex16(stem)) {
              if (WriteDdsFile(out_pack_dir / (stem + ".dds"), *bytes)) ++mod_copied_dds;
              return;
            }
            const std::string parent = LowerAscii(PathParentName(vpath));
            std::string dict;
            if (EndsWithNoCase(parent, ".wtd") || EndsWithNoCase(parent, ".xtd")) {
              dict = parent.substr(0, parent.size() - 4);
            }
            const std::string tex = NormalizeTexName(PathBaseName(vpath));
            const std::vector<uint64_t>* targets = nullptr;
            if (!dict.empty()) {
              auto pit = idx.by_pair.find(dict + "\n" + tex);
              if (pit != idx.by_pair.end()) targets = &pit->second;
            }
            if (!targets) {
              auto tit = idx.by_tex.find(tex);
              if (tit != idx.by_tex.end() && tit->second.size() == 1) targets = &tit->second;
            }
            if (!targets || targets->empty()) {
              ++mod_skipped;
              return;
            }
            for (uint64_t h : *targets) {
              if (WriteDdsFile(out_pack_dir / fmt::format("{:016x}.dds", h), *bytes)) {
                ++mod_converted;
              }
            }
          }
        },
        cancel);
  }

  result.textures_converted += mod_converted;
  result.textures_copied_dds += mod_copied_dds;
  result.textures_skipped += mod_skipped;
}

}  // namespace

fs::path TextureModImportFolder(const fs::path& app) { return app / "mods"; }

fs::path TexturePacksFolder(const fs::path& app) { return app / "textures"; }

int PendingTextureModImports(const fs::path& app) {
  int n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(TextureModImportFolder(app), ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!IsIgnoredModTopEntry(*it)) ++n;
  }
  return n;
}

TextureModImportResult ImportTextureMods(const fs::path& app, const fs::path& liberty_root,
                                         uint32_t max_size,
                                         const TextureModProgressFn& progress,
                                         const std::atomic<bool>* cancel) {
  TextureModImportResult result;
  const fs::path mods_dir = TextureModImportFolder(app);
  const fs::path packs_dir = TexturePacksFolder(app);
  const fs::path game_dir = liberty_root / "game";

  std::error_code ec;
  fs::create_directories(mods_dir, ec);
  fs::create_directories(packs_dir, ec);

  std::vector<fs::path> items;
  for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!IsIgnoredModTopEntry(*it)) items.push_back(it->path());
  }
  std::sort(items.begin(), items.end());
  if (items.empty()) return result;

  AesContext aes;
  aes.Load(app, liberty_root);

  XboxTextureIndex idx;
  const fs::path cache_path = packs_dir / ".xbox_index.bin";
  const fs::path temp_dir = packs_dir;
  if (!BuildXboxIndex(game_dir, cache_path, aes, temp_dir, idx, progress, cancel)) {
    result.cancelled = IsCancelled(cancel);
    return result;
  }
  if (idx.by_tex.empty()) {
    result.errors.push_back("No Xbox 360 game textures found to match mods against. Install GTA IV first.");
    return result;
  }

  for (size_t i = 0; i < items.size(); ++i) {
    if (IsCancelled(cancel)) {
      result.cancelled = true;
      break;
    }
    const fs::path& item = items[i];
    const std::string item_name = item.filename().string();
    const std::string pack_folder = SanitizePackFolderName(item);
    const fs::path out_pack = packs_dir / pack_folder;

    const int before_total = result.textures_converted + result.textures_copied_dds;
    const size_t errors_before = result.errors.size();
    ProcessSingleModItem(item, out_pack, idx, aes, temp_dir, max_size, static_cast<int>(i),
                         static_cast<int>(items.size()), progress, cancel, result);
    if (IsCancelled(cancel)) {
      result.cancelled = true;
      break;
    }

    const int written_this = (result.textures_converted + result.textures_copied_dds) - before_total;
    if (written_this > 0) {
      ++result.mods_processed;
      result.mod_names.push_back(item_name);
      const fs::path done_dir = mods_dir / "done";
      fs::create_directories(done_dir, ec);
      fs::path done_dst = done_dir / item_name;
      if (fs::exists(done_dst, ec)) fs::remove_all(done_dst, ec);
      fs::rename(item, done_dst, ec);
    } else if (result.errors.size() == errors_before) {
      result.errors.push_back(item_name + ": no matching GTA IV textures found inside");
    }
    REXLOG_INFO("PS5 texture import: {} -> {} textures ({})", item_name, written_this,
                out_pack.string());
  }

  if (progress) {
    progress({"Finishing...",
              fmt::format("{} textures ready in textures/",
                          result.textures_converted + result.textures_copied_dds),
              static_cast<int>(items.size()), static_cast<int>(items.size())});
  }
  return result;
}

}  // namespace gta4ps5

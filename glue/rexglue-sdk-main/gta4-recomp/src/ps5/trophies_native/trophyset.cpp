// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Changed for the port: in the
// recomptrophy namespace; stb_image static (the port has its own copy built
// elsewhere); miniz from third_party/.
//
// See trophyset.h.

#include "trophyset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"  // (static: the parts not used)
#include "stb_image.h"
#pragma clang diagnostic pop

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_SDL_NOUNUSED
// (SDL3's copy of miniz names SDL's allocator)
#include <cstdlib>
#define SDL_malloc malloc
#define SDL_free free
#define SDL_realloc realloc
#include "third_party/miniz.h"

namespace recomptrophy::trophyset {
namespace {

using Bytes = std::vector<uint8_t>;

void Sha1(const uint8_t* data, size_t size, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  auto block = [&](const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      const uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
  };
  size_t at = 0;
  for (; at + 64 <= size; at += 64) block(data + at);
  uint8_t tail[128] = {};
  const size_t rest = size - at;
  std::memcpy(tail, data + at, rest);
  tail[rest] = 0x80;
  const size_t tail_size = rest + 9 <= 64 ? 64 : 128;
  const uint64_t bits = uint64_t(size) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = uint8_t(bits >> (i * 8));
  for (size_t o = 0; o < tail_size; o += 64) block(tail + o);
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = uint8_t(h[i] >> 24), out[i * 4 + 1] = uint8_t(h[i] >> 16);
    out[i * 4 + 2] = uint8_t(h[i] >> 8), out[i * 4 + 3] = uint8_t(h[i]);
  }
}

// Catmull-Rom scaling of RGBA pixels.
Bytes Scale(const uint8_t* src, int w, int h, int size) {
  auto weight = [](float x) {
    x = std::fabs(x);
    if (x < 1) return 1.5f * x * x * x - 2.5f * x * x + 1;
    if (x < 2) return -0.5f * x * x * x + 2.5f * x * x - 4 * x + 2;
    return 0.0f;
  };
  Bytes dst(size_t(size) * size * 4);
  for (int y = 0; y < size; ++y) {
    const float fy = (y + 0.5f) * h / size - 0.5f;
    const int iy = int(std::floor(fy));
    for (int x = 0; x < size; ++x) {
      const float fx = (x + 0.5f) * w / size - 0.5f;
      const int ix = int(std::floor(fx));
      float sum[4] = {}, total = 0;
      for (int j = -1; j <= 2; ++j) {
        const float wy = weight(fy - (iy + j));
        const int sy = std::clamp(iy + j, 0, h - 1);
        for (int i = -1; i <= 2; ++i) {
          const float wgt = wy * weight(fx - (ix + i));
          const uint8_t* p = &src[(size_t(sy) * w + std::clamp(ix + i, 0, w - 1)) * 4];
          for (int c = 0; c < 4; ++c) sum[c] += p[c] * wgt;
          total += wgt;
        }
      }
      for (int c = 0; c < 4; ++c) {
        dst[(size_t(y) * size + x) * 4 + c] = uint8_t(std::clamp(sum[c] / total + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  return dst;
}

// A PNG scaled to size x size (opaque, as the console's trophy pictures).
Bytes Picture(const Bytes& png, int size) {
  if (png.empty()) return {};
  int w = 0, h = 0, n = 0;
  uint8_t* rgba = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
  if (!rgba) return {};
  Bytes scaled = (w == size && h == size) ? Bytes(rgba, rgba + size_t(w) * h * 4) : Scale(rgba, w, h, size);
  stbi_image_free(rgba);
  for (size_t i = 3; i < scaled.size(); i += 4) scaled[i] = 255;
  size_t length = 0;
  void* out = tdefl_write_image_to_png_file_in_memory_ex(scaled.data(), size, size, 4, size * 4, &length, 6, MZ_FALSE,
                                                        nullptr, 0, nullptr, 0);
  if (!out) return {};
  Bytes result(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + length);
  mz_free(out);
  return result;
}

std::string Json(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') out += '\\', out += char(c);
    else if (c < 0x20) out += ' ';
    else out += char(c);
  }
  return out;
}

void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v)); }

bool WriteFile(const std::string& path, const Bytes& data) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = std::fclose(f) == 0 && ok;
  return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

std::string SetId(uint32_t title_id) {
  if (title_id == 0x454109AB) return "NPWR99990_00";
  char id[16];
  std::snprintf(id, sizeof(id), "NPWR%05u_00", 90000u + title_id % 9989u);
  return id;
}

uint32_t TrophyId(const game::Info& info, uint32_t achievement_id) {
  uint32_t rank = 0;
  bool found = false;
  for (const auto& a : info.achievements) {
    if (a.id < achievement_id) ++rank;
    found = found || a.id == achievement_id;
  }
  return found ? rank + 1 : 0;
}

bool Build(const game::Info& info, const std::string& set_id, const std::string& title_name, const Bytes& icon_png,
           const std::string& ucp_path, const std::string& list_path, float* progress) {
  struct File {
    std::string name;
    Bytes data;
  };
  std::vector<File> files;
  Bytes icon = Picture(icon_png, 512);
  if (icon.empty()) icon = Picture(info.icon, 512);
  files.push_back({"icon0_en-US.png", icon});
  files.push_back({"trop0000.png", icon});

  auto grade_of = [](uint32_t gamerscore) { return gamerscore <= 10 ? 4 : gamerscore <= 25 ? 3 : 2; };
  static const char kGradeLetters[] = "?PGSB";
  std::ostringstream conf, list, meta[2];
  conf << "{\"schemaVersion\":\"1.00\",\"trophyDefinitionRevision\":1,\"platform\":[\"PS5\"],"
       << "\"trophyNpCommId\":\"" << set_id << "\",\"udsNpCommId\":\"" << set_id << "\","
       << "\"trophySetVersion\":\"01.00\",\"defaultLanguage\":\"en-US\",\"languages\":[\"en-US\",\"pt-BR\"],"
       << "\"trophies\":[{\"id\":\"0000\",\"hidden\":false,\"grade\":\"P\",\"hasReward\":false}";
  list << "0 1 0 -1\n";
  for (int l = 0; l < 2; ++l) {
    meta[l] << "{\"schemaVersion\":\"0.90\",\"trophyDefinitionRevision\":1,\"trophySetVersion\":\"01.00\","
            << "\"trophyNpCommId\":\"" << set_id << "\",\"metadata\":{\"titleMetadata\":{\"name\":\""
            << Json(title_name) << "\"},\"trophyMetadata\":[{\"id\":\"0000\",\"name\":\""
            << Json(title_name) << "\",\"detail\":\""
            << (l ? "Desbloqueie todos os troféus" : "Unlock all trophies") << "\"}";
  }
  size_t done = 0;
  for (const auto& a : info.achievements) {
    const uint32_t trophy = TrophyId(info, a.id);
    char tid[8];
    std::snprintf(tid, sizeof(tid), "%04u", trophy);
    const int grade = grade_of(a.gamerscore);
    Bytes picture = Picture(a.picture, 512);
    files.push_back({std::string("trop") + tid + ".png", picture.empty() ? icon : picture});
    conf << ",{\"id\":\"" << tid << "\",\"hidden\":" << (a.hidden ? "true" : "false") << ",\"grade\":\""
         << kGradeLetters[grade] << "\",\"hasReward\":false,\"platinumTrophyId\":\"0000\","
         << "\"unlockCondition\":{\"udsStatId\":\"" << trophy
         << "\",\"comparator\":\"ge\",\"targetValue\":\"1\",\"progressive\":false,"
         << "\"locations\":[{\"unlockType\":\"client\",\"platform\":[\"PS5\"]}]}}";
    list << trophy << ' ' << grade << ' ' << (a.hidden ? 1 : 0) << " 0\n";
    for (int l = 0; l < 2; ++l) {
      meta[l] << ",{\"id\":\"" << tid << "\",\"name\":\"" << Json(a.name[l]) << "\",\"detail\":\""
              << Json(a.detail[l]) << "\"}";
    }
    if (progress) *progress = float(++done) / float(info.achievements.size()) * 0.95f;
  }
  conf << "]}";
  for (auto& m : meta) m << "]}}";
  auto text = [](const std::string& s) { return Bytes(s.begin(), s.end()); };
  files.push_back({"tropconf.json", text(conf.str())});
  files.push_back({"tropmeta_en-US.json", text(meta[0].str())});
  files.push_back({"tropmeta_pt-BR.json", text(meta[1].str())});

  const size_t table_end = 0x60 + files.size() * 0x40;
  size_t pos = (table_end + 0xF) & ~size_t(0xF);
  std::vector<size_t> offsets;
  for (const auto& f : files) {
    offsets.push_back(pos);
    pos = (pos + f.data.size() + 0x10) & ~size_t(0xF);
  }
  Bytes ucp(pos);
  for (size_t i = 0; i < files.size(); ++i) {
    uint8_t* e = &ucp[0x60 + i * 0x40];
    std::memcpy(e, files[i].name.data(), std::min<size_t>(files[i].name.size(), 31));
    Put64(e + 32, offsets[i]);
    Put64(e + 40, files[i].data.size());
    if (!files[i].data.empty()) std::memcpy(&ucp[offsets[i]], files[i].data.data(), files[i].data.size());
  }
  Put32(&ucp[0], 0xB228C60A);
  Put32(&ucp[4], 1);
  Put64(&ucp[8], ucp.size());
  Put32(&ucp[0x10], uint32_t(files.size()));
  Put32(&ucp[0x14], 0x40);
  Sha1(ucp.data(), ucp.size(), &ucp[0x1C]);
  const std::string l = list.str();
  const bool ok = WriteFile(ucp_path, ucp) && WriteFile(list_path, Bytes(l.begin(), l.end()));
  if (progress) *progress = 1.0f;
  return ok;
}

}  // namespace recomptrophy::trophyset

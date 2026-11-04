// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Unchanged.
//
// The player's progress in the game's trophy set, in the console's own file
// (/user/home/USER/trophy2/nobackup/data/SET/TRPTITLE.DAT, "T2PD"), so the
// game shows in the PS5's trophy list with what was earned. Part of the
// relay (relay.c includes it, with g_set: the set's id, g_user: the player).
//
// The file: a 0x40 byte header, then blocks of 0x400 bytes each followed by
// its SHA-256 (zero for an all-zero block; no key). Records (type, size, ...)
// run through the data without the hashes. An unlock is recorded as the
// console records one (as Trophy Manager, github.com/kauansilvalopes/trophy,
// found from real files): the trophy's 0x800 state record, the earned bitmaps
// and the latest-earned times of the 0x600/0x700 records, the 0x2 header's
// latest event time and two revision counters.
//
// A first file (BuildProgressFile): made from another of the player's games'
// files (their account's fields and the console's layout), with the game's
// trophies (FOLDER/trophies.txt, from the app: src/ps5_trophy_set.h) and its
// trophy set's SHA-1 (FOLDER/TROPHY.UCP's), nothing earned
// (tools/trophy2/make_trptitle.py makes the same).

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TRP_HEAD 0x40
#define TRP_CHUNK 0x400
#define TRP_STRIDE 0x420
#define TRP_MAX (256 * 1024)

// ---- SHA-256 ---------------------------------------------------------------

static const uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha256Block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
    const uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

// (sizes a multiple of 64 bytes, as the file's blocks)
static void Sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  for (size_t at = 0; at < size; at += 64) {
    Sha256Block(h, data + at);
  }
  uint8_t pad[64] = {0x80};
  const uint64_t bits = (uint64_t)size * 8;
  for (int i = 0; i < 8; ++i) pad[56 + i] = (uint8_t)(bits >> (56 - i * 8));
  Sha256Block(h, pad);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8), out[i * 4 + 3] = (uint8_t)h[i];
  }
}

// ---- the file ----------------------------------------------------------------

static uint32_t Be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
static void Put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void Put64(uint8_t* p, uint64_t v) { Put32(p, (uint32_t)(v >> 32)), Put32(p + 4, (uint32_t)v); }

// The file read as its data without the hashes (`stream`); its size, or 0.
static size_t ReadTrophyFile(const char* path, uint8_t* stream) {
  static uint8_t file[TRP_MAX];
  FILE* in = fopen(path, "rb");
  if (!in) return 0;
  const size_t n = fread(file, 1, sizeof(file), in);
  fclose(in);
  if (n < TRP_HEAD || (n - TRP_HEAD) % TRP_STRIDE != 0 || memcmp(file, "T2PD", 4) != 0) return 0;
  memcpy(stream, file, TRP_HEAD);
  size_t s = TRP_HEAD;
  for (size_t o = TRP_HEAD; o + TRP_STRIDE <= n; o += TRP_STRIDE, s += TRP_CHUNK) {
    memcpy(stream + s, file + o, TRP_CHUNK);
  }
  return s;
}

// Written back with its hashes (a new file renamed in when whole).
static bool WriteTrophyFile(const char* path, const uint8_t* stream, size_t size) {
  static uint8_t file[TRP_MAX];
  memcpy(file, stream, TRP_HEAD);
  size_t n = TRP_HEAD;
  for (size_t o = TRP_HEAD; o + TRP_CHUNK <= size; o += TRP_CHUNK) {
    memcpy(file + n, stream + o, TRP_CHUNK);
    bool any = false;
    for (size_t i = 0; i < TRP_CHUNK && !any; ++i) any = stream[o + i] != 0;
    if (any) {
      Sha256(stream + o, TRP_CHUNK, file + n + TRP_CHUNK);
    } else {
      memset(file + n + TRP_CHUNK, 0, 32);
    }
    n += TRP_STRIDE;
  }
  char tmp[300];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* out = fopen(tmp, "wb");
  if (!out) return false;
  bool ok = fwrite(file, 1, n, out) == n;
  ok = fclose(out) == 0 && ok;
  if (ok) {
    chmod(tmp, 0770);
    ok = rename(tmp, path) == 0;
  }
  if (!ok) remove(tmp);
  return ok;
}

// The first record of this type and size (from the end of the section
// directory, in 16 byte steps), or 0.
static size_t FindRecord(const uint8_t* s, size_t size, uint32_t type, uint32_t record_size, size_t from) {
  for (size_t o = from ? from : 0x190; o + 0x10 <= size; o += 16) {
    if (Be32(s + o) == type && Be32(s + o + 4) == record_size) return o;
  }
  return 0;
}

// Trophy `id` recorded as earned at `when` (microseconds since 0001-01-01),
// unless it is: true when the file changed.
static bool UnlockInStream(uint8_t* s, size_t size, uint32_t id, uint64_t when) {
  const size_t h = FindRecord(s, size, 0x2, 0x60, 0), r6 = FindRecord(s, size, 0x600, 0x1f0, 0),
               r7 = FindRecord(s, size, 0x700, 0xb0, 0);
  if (!h || !r6 || !r7 || id >= 128) return false;
  size_t o = 0;
  for (size_t at = FindRecord(s, size, 0x800, 0x50, 0); at; at = FindRecord(s, size, 0x800, 0x50, at + 0x60)) {
    if (Be32(s + at + 0x10) == id) {
      o = at;
      break;
    }
  }
  if (!o || o + 0x60 > size || Be32(s + o + 0x14) == 0x11) return false;
  static const uint8_t kTrailer[16] = {0x55, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 1};
  Put32(s + o + 0x14, 0x11);
  Put32(s + o + 0x18, 0x2000);
  Put64(s + o + 0x20, when);
  Put64(s + o + 0x30, when);
  memcpy(s + o + 0x40, kTrailer, 16);
  const size_t bitmaps[] = {r6 + 0x80, r6 + 0x180, r7 + 0x40};
  for (int i = 0; i < 3; ++i) s[bitmaps[i] + id / 8] |= (uint8_t)(1u << (id % 8));
  uint64_t last = Be64(s + r6 + 0x40);
  if (when > last) last = when;
  const size_t lasts[] = {r6 + 0x40, r6 + 0x70, r7 + 0x20};
  for (int i = 0; i < 3; ++i) Put64(s + lasts[i], last);
  if (when > Be64(s + h + 0x20)) {
    Put64(s + h + 0x20, when);
    Put64(s + h + 0x28, 0);
  }
  Put64(s + h + 0x18, Be64(s + h + 0x18) + 2);
  Put64(s + r6 + 0x10, Be64(s + r6 + 0x10) + 1);
  return true;
}


// ---- a first file ------------------------------------------------------------

// The record of this type in a stream's section directory (0x40: "T2TD", the
// count at 0x48, 0x20 byte entries from 0x70: type, size, 1, count, offset
// from 0x40): its offset and count, or false.
static bool DirRecord(const uint8_t* s, uint32_t type, uint32_t* size, uint32_t* count, size_t* offset) {
  if (memcmp(s + 0x40, "T2TD", 4) != 0) return false;
  const uint32_t n = Be32(s + 0x48);
  for (uint32_t i = 0; i < n && i < 16; ++i) {
    const uint8_t* e = s + 0x70 + i * 0x20;
    if (Be32(e) == type) {
      *size = Be32(e + 4), *count = Be32(e + 12), *offset = 0x40 + (size_t)Be64(e + 16);
      return true;
    }
  }
  return false;
}

static void PutMask(uint8_t* p, const uint8_t* ids, int n) {
  memset(p, 0, 16);
  for (int i = 0; i < n; ++i) p[ids[i] / 8] |= (uint8_t)(1u << (ids[i] % 8));
}

// The game's progress file in `s` from `t` (another game's): its size, or 0.
static size_t BuildProgressFile(const uint8_t* t, size_t t_size, uint8_t* s) {
  // the trophies: "ID GRADE HIDDEN PLATINUM_ID"
  uint8_t ids[128], grades[128], hidden[128];
  int n = 0;
  FILE* in = fopen(TOAST_FOLDER "/trophies.txt", "r");
  if (!in) return 0;
  unsigned id, grade, hide;
  int plat;
  while (n < 128 && fscanf(in, "%u %u %u %d", &id, &grade, &hide, &plat) == 4) {
    if (id < 128 && grade >= 1 && grade <= 4) ids[n] = (uint8_t)id, grades[n] = (uint8_t)grade, hidden[n] = (uint8_t)hide, ++n;
  }
  fclose(in);
  uint8_t ucp_sha1[20];
  in = fopen(TOAST_FOLDER "/TROPHY.UCP", "rb");
  if (!in) return 0;
  uint8_t head[0x30];
  const bool have_head = fread(head, 1, sizeof(head), in) == sizeof(head);
  fclose(in);
  if (!n || !have_head) return 0;
  memcpy(ucp_sha1, head + 0x1C, 20);

  static const uint32_t kOrder[9] = {0x1, 0x2, 0x100, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800};
  uint32_t sizes[9], counts[9];
  size_t offsets[9];
  for (int i = 0; i < 9; ++i) {
    if (!DirRecord(t, kOrder[i], &sizes[i], &counts[i], &offsets[i]) || !counts[i] ||
        offsets[i] + (size_t)(sizes[i] + 0x10) * counts[i] > t_size) {
      return 0;
    }
  }
  if (sizes[5] != 0xc0 || sizes[6] != 0x1f0 || sizes[7] != 0xb0 || sizes[8] != 0x50 || sizes[3] != 0x480 ||
      sizes[4] != 0x90 || sizes[2] != 0x40) {
    return 0;  // (a layout this doesn't know)
  }
  // the 0x500 records to copy: the template's platinum's and another's
  size_t plat_rec = 0, other_rec = 0;
  for (uint32_t k = 0; k < counts[5]; ++k) {
    const size_t r = offsets[5] + k * 0xd0;
    if (Be32(t + r + 0x14) == 1) {
      if (!plat_rec) plat_rec = r;
    } else if (!other_rec) {
      other_rec = r;
    }
  }
  if (!other_rec) return 0;

  memset(s, 0, TRP_MAX);
  memcpy(s, t, 0x190);
  size_t pos = 0x190;
  struct { uint32_t count; } layout[9];
  for (int i = 0; i < 9; ++i) layout[i].count = (i == 5 || i == 8) ? (uint32_t)n : 1;
  Put32(s + 0x48, 9);
  memset(s + 0x70, 0, 0x190 - 0x70);
  size_t at[9];
  for (int i = 0; i < 9; ++i) {
    uint8_t* e = s + 0x70 + i * 0x20;
    Put32(e, kOrder[i]), Put32(e + 4, sizes[i]), Put32(e + 8, 1), Put32(e + 12, layout[i].count);
    Put64(e + 16, pos - 0x40);
    at[i] = pos;
    pos += (size_t)(sizes[i] + 0x10) * layout[i].count;
  }
  const size_t blocks = (pos - 0x40 + TRP_CHUNK - 1) / TRP_CHUNK;
  const size_t size = 0x40 + blocks * TRP_CHUNK;
  if (size > TRP_MAX) return 0;
  // the records kept as they are (0x1, 0x2: the account's), or adjusted
  for (int i = 0; i < 5; ++i) memcpy(s + at[i], t + offsets[i], sizes[i] + 0x10);
  memcpy(s + at[2] + 0x10, ucp_sha1, 20);
  uint8_t* r3 = s + at[3];
  memset(r3 + 0x10, 0, 16), memcpy(r3 + 0x10, g_set, strlen(g_set));
  memset(r3 + 0xc0, 0, 16), memcpy(r3 + 0xc0, g_set, strlen(g_set));
  Put32(r3 + 0xe8, 1);
  PutMask(r3 + 0x100, ids, n);
  uint8_t secret[128];
  int k = 0;
  for (int i = 0; i < n; ++i) if (hidden[i]) secret[k++] = ids[i];
  PutMask(r3 + 0x180, secret, k);  // (the hidden ones)
  for (int g = 1; g <= 4; ++g) {
    uint8_t of_grade[128];
    int m = 0;
    for (int i = 0; i < n; ++i) if (grades[i] == g) of_grade[m++] = ids[i];
    PutMask(r3 + 0x200 + (g - 1) * 0x80, of_grade, m);
  }
  PutMask(s + at[4] + 0x20, ids, n);
  uint8_t others[128];
  int m = 0;
  for (int i = 0; i < n; ++i) if (grades[i] != 1) others[m++] = ids[i];
  for (int i = 0; i < n; ++i) {
    uint8_t* r = s + at[5] + i * 0xd0;
    memcpy(r, t + (grades[i] == 1 && plat_rec ? plat_rec : other_rec), 0xd0);
    Put32(r + 0x10, ids[i]), Put32(r + 0x14, grades[i]), Put32(r + 0x18, hidden[i]);
    if (grades[i] == 1) PutMask(r + 0x50, others, m);
  }
  memcpy(s + at[6], t + offsets[6], 0x200);
  memcpy(s + at[7], t + offsets[7], 0xc0);
  uint8_t* r6 = s + at[6];
  uint8_t* r7 = s + at[7];
  memset(r6 + 0x80, 0, 16), memset(r6 + 0x180, 0, 16), memset(r7 + 0x40, 0, 16);
  memset(r6 + 0x40, 0, 8), memset(r6 + 0x70, 0, 8), memset(r7 + 0x20, 0, 8);
  for (int i = 0; i < n; ++i) {
    uint8_t* r = s + at[8] + i * 0x60;
    Put32(r, 0x800), Put32(r + 4, 0x50), Put32(r + 0x10, ids[i]);
  }
  Put32(s + 0x24, (uint32_t)blocks);
  return size;
}


// FOLDER/unlocked.txt (from the app: "ID UNIXTIME" lines, every trophy the
// player has) recorded in the player's file, made first when there's none.
// Whether the file's trophies (its 0x800 records) are the set's (trophies.txt).
static bool SameTrophies(const uint8_t* s, size_t size) {
  uint8_t want[128] = {0}, have[128] = {0};
  int n_want = 0, n_have = 0;
  FILE* in = fopen(TOAST_FOLDER "/trophies.txt", "r");
  if (!in) return true;
  unsigned id, grade, hide;
  int plat;
  while (fscanf(in, "%u %u %u %d", &id, &grade, &hide, &plat) == 4) {
    if (id < 128 && !want[id]) want[id] = 1, ++n_want;
  }
  fclose(in);
  for (size_t at = FindRecord(s, size, 0x800, 0x50, 0); at; at = FindRecord(s, size, 0x800, 0x50, at + 0x60)) {
    const uint32_t tid = Be32(s + at + 0x10);
    if (tid < 128 && !have[tid]) have[tid] = 1, ++n_have;
  }
  return n_want == n_have && memcmp(want, have, sizeof(want)) == 0;
}

static void RecordTrophies(void) {
  g_progress = NULL;
  FILE* list = fopen(TOAST_FOLDER "/unlocked.txt", "r");
  if (!list) return;
  const int user = g_user;
  char home[64], folder[160], path[200];
  snprintf(home, sizeof(home), "/user/home/%08x/trophy2/nobackup/data", (unsigned)user);
  snprintf(folder, sizeof(folder), "%s/%s", home, g_set);
  snprintf(path, sizeof(path), "%s/TRPTITLE.DAT", folder);
  static uint8_t s[TRP_MAX];
  size_t size = ReadTrophyFile(path, s);
  bool changed = false;
  if (size && !SameTrophies(s, size)) {
    // made for another list of trophies (an earlier set of the game's): made
    // again, the old one kept beside it
    char old_path[220];
    snprintf(old_path, sizeof(old_path), "%s.old", path);
    rename(path, old_path);
    size = 0;
  }
  if (size) {
    // a new trophy set (the app made it again): its SHA-1 in the 0x100 record
    uint32_t record_size, count;
    size_t record;
    FILE* ucp = fopen(TOAST_FOLDER "/TROPHY.UCP", "rb");
    uint8_t head[0x30];
    if (ucp && fread(head, 1, sizeof(head), ucp) == sizeof(head) &&
        DirRecord(s, 0x100, &record_size, &count, &record) && record + 0x24 <= size &&
        memcmp(s + record + 0x10, head + 0x1C, 20) != 0) {
      memcpy(s + record + 0x10, head + 0x1C, 20);
      changed = true;
    }
    if (ucp) fclose(ucp);
  }
  if (!size) {
    // made from another of the player's games' files
    static uint8_t other[TRP_MAX];
    DIR* dir = opendir(home);
    struct dirent* entry;
    while (dir && !size && (entry = readdir(dir))) {
      if (strncmp(entry->d_name, "NPWR", 4) != 0 || strcmp(entry->d_name, g_set) == 0) continue;
      char other_path[300];
      snprintf(other_path, sizeof(other_path), "%s/%s/TRPTITLE.DAT", home, entry->d_name);
      const size_t other_size = ReadTrophyFile(other_path, other);
      if (other_size) size = BuildProgressFile(other, other_size, s);
    }
    if (dir) closedir(dir);
    if (!size) {
      fclose(list);
      g_progress = "no-template";  // (no other game's file: the player's list can't show it)
      return;
    }
    g_progress = "created";
    mkdir(home, 0770);
    mkdir(folder, 0770);
    chmod(folder, 0770);
    changed = true;
  }
  unsigned id;
  long long unix_time;
  while (fscanf(list, "%u %lld", &id, &unix_time) == 2) {
    const uint64_t when = (uint64_t)(unix_time + 62135596800LL) * 1000000ULL;
    changed = UnlockInStream(s, size, id, when) || changed;
  }
  fclose(list);
  if (changed && !WriteTrophyFile(path, s, size)) g_progress = "write-failed";
  if (!g_progress) g_progress = changed ? "updated" : "unchanged";
}

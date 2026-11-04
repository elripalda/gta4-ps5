// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Unchanged; built by this
// folder's Makefile and sent by the game (src/ps5/trophies_native/install.cpp).
//
// RecompTrophy's watcher: sent by the app (src/install.cpp) to the payload
// loader on port 9021, it stays loaded and, every 2 seconds, looks at the
// achievement files of the games whose trophies the app made
// (WATCH/SET/game.txt: "SET USER" and the game's name; map.txt: "ACHIEVEMENT
// TROPHY GRADE NAME" lines; sources.txt: the files - a ReXGlue port's
// achievements .toml, an Xbox profile's TITLEID.gpd). Each achievement found
// earned there and not yet in the player's progress file is recorded in it
// (trophy_progress.c) and shown with the console's own trophy notification
// (its picture from the set: trp2://localfile/SET/trophies/N/icon), the
// platinum once all are.
//
// One runs at a time: WATCH/alive holds the time it last looked; another
// finding it fresh stops. A game is left alone when turned off in the app
// (WATCH/SET/off) or when its port shows its own notifications
// (/data/trophies360/native/SET, left by the port).

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// (ROOT: a folder standing for the console's root, in tests off it)
#ifndef ROOT
#define ROOT ""
#endif
#define TOAST_FOLDER ROOT "/data/trophies360/job"
#define WATCH ROOT "/data/trophies360/watch"

static char g_set[32];
static int g_user = -1;
static const char* g_progress;

#include "trophy_progress.c"

int sceNotificationSend(int user_id, bool is_logged, const char* payload);

#define MAX_GAMES 64
#define MAX_SOURCES 8
#define MAX_TROPHIES 128

struct Game {
  char set[32];
  long long seen[MAX_SOURCES];  // the sources' modification times last looked at
};
static struct Game games[MAX_GAMES];
static int game_count;

static struct Game* GameOf(const char* set) {
  for (int i = 0; i < game_count; ++i) {
    if (strcmp(games[i].set, set) == 0) return &games[i];
  }
  if (game_count == MAX_GAMES) return NULL;
  struct Game* g = &games[game_count++];
  memset(g, 0, sizeof(*g));
  snprintf(g->set, sizeof(g->set), "%s", set);
  return g;
}

// ---- the achievements earned in a source ----------------------------------------

// earned[achievement] = unix time (0: not earned)
static void EarnedInToml(const char* path, long long* earned) {
  FILE* f = fopen(path, "r");
  if (!f) return;
  char line[256];
  unsigned id = 0;
  bool in_entry = false;
  while (fgets(line, sizeof(line), f)) {
    unsigned n;
    unsigned long long filetime;
    if (sscanf(line, "[unlocked.%u]", &n) == 1) {
      id = n, in_entry = true;
    } else if (in_entry && sscanf(line, " filetime = %llu", &filetime) == 1) {
      if (id < 256) earned[id] = filetime ? (long long)(filetime / 10000000ULL) - 11644473600LL : (long long)time(NULL);
      in_entry = false;
    }
  }
  fclose(f);
}

static void EarnedInGpd(const char* path, long long* earned) {
  static uint8_t d[1 << 20];
  FILE* f = fopen(path, "rb");
  if (!f) return;
  const size_t n = fread(d, 1, sizeof(d), f);
  fclose(f);
  if (n < 24 || memcmp(d, "XDBF", 4) != 0) return;
  const uint32_t table = Be32(d + 8), used = Be32(d + 12), free_table = Be32(d + 16);
  const size_t data = 24 + (size_t)table * 18 + (size_t)free_table * 8;
  for (uint32_t i = 0; i < used && 24 + (i + 1) * 18 <= n; ++i) {
    const uint8_t* e = d + 24 + i * 18;
    if ((e[0] << 8 | e[1]) != 1) continue;
    const size_t at = data + Be32(e + 10), size = Be32(e + 14);
    if (size < 28 || at + size > n) continue;
    const uint32_t id = Be32(d + at + 4), flags = Be32(d + at + 16);
    const uint64_t filetime = (uint64_t)Be32(d + at + 20) << 32 | Be32(d + at + 24);
    if ((flags & 0x20000) && id < 256 && !earned[id]) {
      earned[id] = filetime ? (long long)(filetime / 10000000ULL) - 11644473600LL : (long long)time(NULL);
    }
  }
}

// ---- the notification ----------------------------------------------------------------

static void JsonText(char* out, size_t size, const char* text) {
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)text; *p && o + 2 < size; ++p) {
    if (*p == '"' || *p == '\\') out[o++] = '\\', out[o++] = (char)*p;
    else if (*p >= 0x20) out[o++] = (char)*p;
  }
  out[o] = 0;
}

static void Notify(const char* set, const char* game, int trophy, int grade, const char* name, int percent,
                   const int unlocked[5]) {
  static const char* const kGradeIcons[5] = {"", "trophy_grade1_platinum", "trophy_grade2_gold",
                                             "trophy_grade3_silver", "trophy_grade4_bronze"};
  char safe_name[256], safe_game[256], json[4096], created[40], icon[96], set_icon[96];
  JsonText(safe_name, sizeof(safe_name), name);
  JsonText(safe_game, sizeof(safe_game), game);
  const time_t now = time(NULL);
  strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S.000Z", gmtime(&now));
  snprintf(icon, sizeof(icon), "trp2://localfile/%s/trophies/%d/icon", set, trophy);
  snprintf(set_icon, sizeof(set_icon), "trp2://localfile/%s/icon", set);
  // the console's own trophy toast (as a PS5 game's: its template, the trophy
  // channel and sound; U+F8E1, the console's font's trophy cup, before the name)
  snprintf(json, sizeof(json),
           "{\"rawData\":{\"bundleName\":\"local:Trophy\",\"channelType\":\"Trophies\",\"isImmediate\":true,"
           "\"priority\":1,\"soundEffect\":\"%s\",\"toastOverwriteType\":\"No\",\"useCaseId\":\"NUC55\","
           "\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{\"parameters\":{\"icon\":\"trophies\"},"
           "\"type\":\"Predefined\"},\"message\":{\"body\":\"Trophy earned!\"}}}},"
           "\"viewData\":{\"icon\":{\"parameters\":{\"url\":\"%s\"},\"type\":\"Url\"},"
           "\"message\":{\"body\":\"\xEF\xA3\xA1 %s\"},"
           "\"parameters\":{\"game\":{\"progressText\":\"%d%%\",\"trophySetIcon\":\"%s\",\"trophySetName\":\"%s\","
           "\"unlockedBronze\":%d,\"unlockedGold\":%d,\"unlockedPlatinum\":%d,\"unlockedSilver\":%d},"
           "\"label\":{\"earned\":\"Earned\",\"progress\":\"Total trophy progress\"},\"secondaryText\":\"%s\","
           "\"trophy\":{\"gradeIcon\":\"%s\",\"name\":\"%s\",\"uri\":\"%s\"},\"useCaseId\":\"NUC55\"},"
           "\"subMessage\":{\"body\":\"Trophy earned!\"}},\"viewTemplateType\":\"InteractiveToastTrophy\"},"
           "\"createdDateTime\":\"%s\",\"localNotificationId\":\"%ld%d\"}",
           grade == 1 ? "psfx_platinum_trophy_toast" : "psfx_trophy_toast", icon, safe_name, percent, set_icon,
           safe_game, unlocked[4], unlocked[2], unlocked[1], unlocked[3], safe_game, kGradeIcons[grade], safe_name,
           icon, created, (long)now, trophy);
  sceNotificationSend(0xFE, true, json);
}

// ---- a game ------------------------------------------------------------------------------

static bool IsEarned(const uint8_t* s, size_t size, uint32_t id) {
  for (size_t at = FindRecord(s, size, 0x800, 0x50, 0); at; at = FindRecord(s, size, 0x800, 0x50, at + 0x60)) {
    if (Be32(s + at + 0x10) == id) return Be32(s + at + 0x14) == 0x11;
  }
  return false;
}

static void Look(const char* folder) {
  char path[300], set[32], name[256] = "";
  unsigned user = 0;
  snprintf(path, sizeof(path), "%s/game.txt", folder);
  FILE* f = fopen(path, "r");
  if (!f) return;
  const bool ok = fscanf(f, "%31s %x", set, &user) == 2;
  if (ok) {
    fgets(name, sizeof(name), f);  // (the rest of the first line)
    if (!fgets(name, sizeof(name), f)) name[0] = 0;
    name[strcspn(name, "\r\n")] = 0;
  }
  fclose(f);
  if (!ok) return;
  // (off: turned off in the app; or the game shows its own trophy
  // notifications, its port having native trophies)
  struct stat skip;
  snprintf(path, sizeof(path), "%s/off", folder);
  if (stat(path, &skip) == 0) return;
  snprintf(path, sizeof(path), ROOT "/data/trophies360/native/%s", set);
  if (stat(path, &skip) == 0) return;
  struct Game* game = GameOf(set);
  if (!game) return;

  // the sources: looked at when one changed
  snprintf(path, sizeof(path), "%s/sources.txt", folder);
  f = fopen(path, "r");
  if (!f) return;
  char sources[MAX_SOURCES][256];
  int source_count = 0;
  bool changed = false;
  while (source_count < MAX_SOURCES && fgets(sources[source_count], sizeof(sources[0]), f)) {
    sources[source_count][strcspn(sources[source_count], "\r\n")] = 0;
    if (!sources[source_count][0]) continue;
    struct stat st;
    const long long mtime = stat(sources[source_count], &st) == 0 ? (long long)st.st_mtime : 0;
    if (mtime != game->seen[source_count]) changed = true;
    game->seen[source_count] = mtime;
    ++source_count;
  }
  fclose(f);
  if (!changed) return;
  static long long earned[256];
  memset(earned, 0, sizeof(earned));
  for (int i = 0; i < source_count; ++i) {
    const size_t n = strlen(sources[i]);
    if (n > 4 && strcmp(sources[i] + n - 4, ".gpd") == 0) EarnedInGpd(sources[i], earned);
    else EarnedInToml(sources[i], earned);
  }

  // the trophies (achievement -> trophy, grade, name)
  static int trophy_of[256], grade_of[MAX_TROPHIES];
  static char names[MAX_TROPHIES][128];
  memset(trophy_of, 0, sizeof(trophy_of));
  memset(grade_of, 0, sizeof(grade_of));
  int count = 0;
  snprintf(path, sizeof(path), "%s/map.txt", folder);
  f = fopen(path, "r");
  if (!f) return;
  char line[300];
  while (fgets(line, sizeof(line), f)) {
    unsigned achievement, trophy;
    int grade, used = 0;
    if (sscanf(line, "%u %u %d %n", &achievement, &trophy, &grade, &used) < 3) continue;
    if (achievement >= 256 || trophy == 0 || trophy >= MAX_TROPHIES) continue;
    trophy_of[achievement] = (int)trophy;
    grade_of[trophy] = grade;
    snprintf(names[trophy], sizeof(names[trophy]), "%s", line + used);
    names[trophy][strcspn(names[trophy], "\r\n")] = 0;
    ++count;
  }
  fclose(f);
  grade_of[0] = 1;
  snprintf(names[0], sizeof(names[0]), "%s", name);

  // the player's progress file
  char home[256], file[300];
  snprintf(home, sizeof(home), ROOT "/user/home/%08x/trophy2/nobackup/data/%s", user, set);
  snprintf(file, sizeof(file), "%s/TRPTITLE.DAT", home);
  static uint8_t s[TRP_MAX];
  const size_t size = ReadTrophyFile(file, s);
  if (!size) return;
  int fresh[MAX_TROPHIES], fresh_count = 0;
  bool all = count > 0;
  for (int a = 0; a < 256; ++a) {
    const int trophy = trophy_of[a];
    if (!trophy) continue;
    if (!earned[a]) {
      all = false;
      continue;
    }
    const uint64_t when = (uint64_t)(earned[a] + 62135596800LL) * 1000000ULL;
    if (UnlockInStream(s, size, (uint32_t)trophy, when)) fresh[fresh_count++] = trophy;
  }
  if (all && !IsEarned(s, size, 0) && UnlockInStream(s, size, 0, (uint64_t)(time(NULL) + 62135596800LL) * 1000000ULL)) {
    fresh[fresh_count++] = 0;
  }
  if (!fresh_count || !WriteTrophyFile(file, s, size)) return;

  // the notifications, with the progress (the PS5's points: 300/90/30/15)
  static const int kPoints[5] = {0, 300, 90, 30, 15};
  int unlocked[5] = {0}, points = 0, total = 0;
  for (int t = 0; t < MAX_TROPHIES; ++t) {
    if (t && !grade_of[t]) continue;
    const int g = grade_of[t];
    total += kPoints[g];
    if (IsEarned(s, size, (uint32_t)t)) points += kPoints[g], ++unlocked[g];
  }
  const int percent = total ? points * 100 / total : 0;
  for (int i = 0; i < fresh_count; ++i) {
    Notify(set, name, fresh[i], grade_of[fresh[i]], names[fresh[i]], percent, unlocked);
    sleep(fresh[i] == 0 ? 1 : 4);  // (one after another, as the console shows them)
  }
}

// Its line in the Payload Manager's autoload list (/data/pldmgr/autoload.txt,
// root's only: the app puts the payload beside it and this the line), so it
// is loaded at each start of the console. Added at the end, the rest kept.
static void AddToAutoload(void) {
  const char* list = ROOT "/data/pldmgr/autoload.txt";
  struct stat st;
  if (stat(ROOT "/data/pldmgr/payloads/local/recomptrophy-watcher/RecompTrophy-Watcher.elf", &st) != 0) return;
  FILE* in = fopen(list, "r");
  if (!in) return;
  char line[256];
  int last = '\n';
  while (fgets(line, sizeof(line), in)) {
    const size_t n = strlen(line);
    if (n) last = (unsigned char)line[n - 1];
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, "local/recomptrophy-watcher") == 0) {
      fclose(in);
      return;
    }
  }
  fclose(in);
  FILE* out = fopen(list, "a");
  if (!out) return;
  fprintf(out, "%slocal/recomptrophy-watcher\n", last == '\n' ? "" : "\n");
  fclose(out);
}

int main(void) {
  AddToAutoload();
  mkdir(ROOT "/data/trophies360", 0777);
  mkdir(WATCH, 0777);
  // (one at a time)
  FILE* alive = fopen(WATCH "/alive", "r");
  if (alive) {
    long long at = 0;
    const bool read = fscanf(alive, "%lld", &at) == 1;
    fclose(alive);
    if (read && llabs((long long)time(NULL) - at) < 15) return 0;
  }
  for (;;) {
    alive = fopen(WATCH "/alive.tmp", "w");
    if (alive) {
      fprintf(alive, "%lld\n", (long long)time(NULL));
      fclose(alive);
      rename(WATCH "/alive.tmp", WATCH "/alive");
    }
    DIR* dir = opendir(WATCH);
    if (dir) {
      struct dirent* entry;
      while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "NPWR", 4) != 0) continue;
        char folder[200];
        snprintf(folder, sizeof(folder), "%s/%s", WATCH, entry->d_name);
        Look(folder);
      }
      closedir(dir);
    }
    sleep(2);
  }
}

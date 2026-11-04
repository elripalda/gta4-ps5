// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Unchanged; built by this
// folder's Makefile and sent by the game (src/ps5/trophies_native/install.cpp).
//
// RecompTrophy's relay: the app (src/install.cpp) sends it to the payload
// loader on port 9021 (etaHEN, elfldr, ...), where, as a process of its own,
// it may write where the console keeps trophies, which an app can't.
//
// JOB/job.txt: "ID SET USER" (the job's id, the trophy set's, the player's user id, hex);
// JOB/TROPHY.UCP: the set, put in /user/trophy2/nobackup/conf/SET unless it's
// there already (by SHA-1);
// JOB/trophies.txt, JOB/unlocked.txt: the player's progress file made (from
// another of their games') or updated (trophy_progress.c);
// JOB/result.txt, written last: "ID SET OK|ERROR SET_STATE PROGRESS_STATE".
// Each time, first: the watcher's line in the Payload Manager's autoload list
// (root's only), when the app put the watcher beside it.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TOAST_FOLDER "/data/trophies360/job"

static char g_set[32];
static int g_user = -1;
static const char* g_progress;

#include "trophy_progress.c"

// The set put in place (a copy, renamed in when whole): "installed",
// "same" (there already) or "failed".
static const char* InstallTrophySet(void) {
  char folder[128], target[192], tmp[200];
  snprintf(folder, sizeof(folder), "/user/trophy2/nobackup/conf/%s", g_set);
  snprintf(target, sizeof(target), "%s/TROPHY.UCP", folder);
  snprintf(tmp, sizeof(tmp), "%s/TROPHY.UCP.tmp", folder);
  uint8_t head_new[0x30], head_old[0x30];
  FILE* in = fopen(TOAST_FOLDER "/TROPHY.UCP", "rb");
  if (!in) return "failed";
  const bool have_new = fread(head_new, 1, sizeof(head_new), in) == sizeof(head_new);
  FILE* old = fopen(target, "rb");
  if (old) {
    const bool same = have_new && fread(head_old, 1, sizeof(head_old), old) == sizeof(head_old) &&
                      memcmp(head_new + 0x1C, head_old + 0x1C, 20) == 0;
    fclose(old);
    if (same) {
      fclose(in);
      return "same";
    }
  }
  fseek(in, 0, SEEK_SET);
  mkdir(folder, 0770);
  chmod(folder, 0770);
  FILE* out = fopen(tmp, "wb");
  bool ok = have_new && out;
  static char buffer[1 << 16];
  size_t n;
  while (ok && (n = fread(buffer, 1, sizeof(buffer), in)) > 0) ok = fwrite(buffer, 1, n, out) == n;
  fclose(in);
  if (out) ok = fclose(out) == 0 && ok;
  if (ok) {
    chmod(tmp, 0770);
    ok = rename(tmp, target) == 0;
  }
  if (!ok) remove(tmp);
  return ok ? "installed" : "failed";
}

#include <time.h>

// Its line in the Payload Manager's autoload list (/data/pldmgr/autoload.txt,
// root's only: the app puts the payload beside it and this the line), so it
// is loaded at each start of the console. Added at the end, the rest kept.
static void AddToAutoload(void) {
  const char* list = "/data/pldmgr/autoload.txt";
  struct stat st;
  if (stat("/data/pldmgr/payloads/local/recomptrophy-watcher/RecompTrophy-Watcher.elf", &st) != 0) return;
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
  FILE* job = fopen(TOAST_FOLDER "/job.txt", "r");
  if (!job) return 0;  // (sent at the app's start: the autoload line only)
  char id[64] = "?";
  unsigned user = 0;
  const bool parsed = fscanf(job, "%63s %31s %x", id, g_set, &user) == 3;
  fclose(job);
  const char* set_state = "failed";
  if (parsed && strncmp(g_set, "NPWR", 4) == 0) {
    g_user = (int)user;
    set_state = InstallTrophySet();
    if (strcmp(set_state, "failed") != 0) RecordTrophies();
  }
  FILE* out = fopen(TOAST_FOLDER "/result.tmp", "w");
  if (out) {
    fprintf(out, "%s %s %s %s %s\n", id, g_set,
            strcmp(set_state, "failed") != 0 && g_progress && strcmp(g_progress, "no-template") != 0 &&
                    strcmp(g_progress, "write-failed") != 0
                ? "OK"
                : "ERROR",
            set_state, g_progress ? g_progress : "none");
    fclose(out);
    rename(TOAST_FOLDER "/result.tmp", TOAST_FOLDER "/result.txt");
  }
  return 0;
}

// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Changed for the port: see
// install.h.
//
// See install.h.

#include "install.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace recomptrophy::install {
namespace {

// [ps5 port] the payloads, from the app's payloads/ folder (never freed: the
// thread using them may still run while the game exits)
std::mutex& payloads_mutex = *new std::mutex;
std::vector<uint8_t>& relay_elf = *new std::vector<uint8_t>;
std::vector<uint8_t>& watcher_elf = *new std::vector<uint8_t>;

std::vector<uint8_t> ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Exists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}

// [ps5 port] With time limits, and a loader that hangs up can't end the game
// (no SIGPIPE).
bool SendPayload(const std::vector<uint8_t>& elf) {
  if (elf.empty()) return false;
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return false;
  timeval limit{};
  limit.tv_sec = 5;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(9021);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  // (connect with a 3 second limit; a plain connect when the socket can't
  // be made non-blocking)
  const int mode = fcntl(sock, F_GETFL, 0);
  const bool nonblocking = mode >= 0 && fcntl(sock, F_SETFL, mode | O_NONBLOCK) == 0;
  bool sent = connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  if (!sent && nonblocking && errno == EINPROGRESS) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    pollfd p = {sock, POLLOUT, 0};
    int ready = -1;
    while (ready < 0) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
      if (left.count() <= 0) break;
      ready = poll(&p, 1, int(left.count()));
      if (ready < 0 && errno != EINTR) break;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    sent = ready == 1 && getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
  }
  if (sent && nonblocking) sent = fcntl(sock, F_SETFL, mode) == 0;
  const uint8_t* at = elf.data();
  const uint8_t* end = elf.data() + elf.size();
  while (sent && at < end) {
    const ssize_t n = send(sock, at, size_t(end - at), flags);
    if (n < 0 && errno == EINTR) continue;
    sent = n > 0;
    at += sent ? n : 0;
  }
  if (sent) shutdown(sock, SHUT_WR);
  close(sock);
  return sent;
}

bool SendRelay() {
  std::vector<uint8_t> elf;
  {
    std::lock_guard<std::mutex> lock(payloads_mutex);
    elf = relay_elf;
  }
  return SendPayload(elf);
}

// Files named `name` under `folder` (to `depth` folders down), skipping the
// big ones that never hold them (a game's files, caches).
void FindFiles(const std::string& folder, const std::string& name, int depth, std::vector<std::string>& out) {
  DIR* dir = opendir(folder.c_str());
  if (!dir) return;
  static const char* const kSkip[] = {"game", "assets", "cache", "shader_cache", "radv-shader-cache", "textures",
                                      "logs", "dlc", "install", "sce_module", "shaders"};
  while (dirent* entry = readdir(dir)) {
    const std::string n = entry->d_name;
    if (n == "." || n == "..") continue;
    const std::string path = folder + "/" + n;
    if (n == name) {
      out.push_back(path);
    } else if (depth > 0 && (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) &&
               std::none_of(std::begin(kSkip), std::end(kSkip), [&](const char* k) { return n == k; })) {
      FindFiles(path, name, depth - 1, out);
    }
  }
  closedir(dir);
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// The achievements earned in an Xbox profile's game file (TITLEID.gpd, as the
// Xbox 360 and its emulators keep them): an XDBF whose section 1 entries are
// the achievements (size, id, picture, gamerscore, flags, unlock time; 0x20000
// in the flags: earned).
std::vector<std::pair<uint32_t, int64_t>> EarnedInGpd(const std::string& path) {
  std::vector<std::pair<uint32_t, int64_t>> out;
  const std::vector<uint8_t> d = ReadAll(path);
  if (d.size() < 24 || std::memcmp(d.data(), "XDBF", 4) != 0) return out;
  const uint32_t table = Be32(&d[8]), used = Be32(&d[12]), free_table = Be32(&d[16]);
  const size_t data = 24 + size_t(table) * 18 + size_t(free_table) * 8;
  for (uint32_t i = 0; i < used && 24 + (i + 1) * 18 <= d.size(); ++i) {
    const uint8_t* e = &d[24 + i * 18];
    if ((e[0] << 8 | e[1]) != 1) continue;
    const size_t at = data + Be32(e + 10), size = Be32(e + 14);
    if (size < 28 || at + size > d.size()) continue;
    const uint8_t* a = &d[at];
    const uint32_t id = Be32(a + 4), flags = Be32(a + 16);
    const uint64_t filetime = uint64_t(Be32(a + 20)) << 32 | Be32(a + 24);
    if (!(flags & 0x20000) || id == 0 || id > 127) continue;
    out.emplace_back(id, filetime ? int64_t(filetime / 10000000ull) - 11644473600ll : int64_t(std::time(nullptr)));
  }
  return out;
}

}  // namespace

void SetPayloads(std::vector<uint8_t> relay, std::vector<uint8_t> watcher) {
  std::lock_guard<std::mutex> lock(payloads_mutex);
  relay_elf = std::move(relay);
  watcher_elf = std::move(watcher);
}

// "titleName" of a param.json (the first: its default language's).
std::string TitleName(const std::string& param) {
  const std::string json(param);
  const size_t key = json.find("\"titleName\"");
  if (key == std::string::npos) return {};
  const size_t open = json.find('"', json.find(':', key) + 1);
  if (open == std::string::npos) return {};
  std::string name;
  for (size_t i = open + 1; i < json.size() && json[i] != '"'; ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) ++i;
    name += json[i];
  }
  return name;
}

// [unlocked.N] / filetime = F (Windows file time) lines of a port's file.
std::vector<std::pair<uint32_t, int64_t>> EarnedInToml(const std::string& path) {
  std::vector<std::pair<uint32_t, int64_t>> out;
  std::ifstream in(path);
  std::string line;
  uint32_t id = 0;
  bool in_entry = false;
  while (std::getline(in, line)) {
    unsigned n = 0;
    unsigned long long filetime = 0;
    if (std::sscanf(line.c_str(), "[unlocked.%u]", &n) == 1) {
      id = n, in_entry = true;
    } else if (in_entry && std::sscanf(line.c_str(), " filetime = %llu", &filetime) == 1) {
      const int64_t unix_time =
          filetime ? int64_t(filetime / 10000000ull) - 11644473600ll : int64_t(std::time(nullptr));
      out.emplace_back(id, unix_time);
      in_entry = false;
    }
  }
  return out;
}

bool FindPort(uint32_t title_id, Port& port) {
  char id[16];
  std::snprintf(id, sizeof(id), "%08X", title_id);
  DIR* dir = opendir("/data/homebrew");
  if (!dir) return false;
  std::vector<std::pair<uint32_t, int64_t>> earned;
  bool found = false, named = false;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    const std::string folder = std::string("/data/homebrew/") + entry->d_name;
    // a ReXGlue port's (UserData/.../achievements/TITLEID.toml: its own
    // folder, as the ports, or deeper, as LibertyRecomp's; made at the first
    // achievement: also known by the achievement list beside it,
    // TITLEID-trophies.txt, or the port's shaders - then the file's path is
    // watched for before it's there)
    std::vector<std::string> tomls, lists, gpds, profiles;
    FindFiles(folder + "/UserData", std::string(id) + ".toml", 4, tomls);
    FindFiles(folder + "/UserData", std::string(id) + "-trophies.txt", 4, lists);
    for (const std::string& l : lists) {
      const std::string t = l.substr(0, l.rfind('/') + 1) + id + ".toml";
      if (std::find(tomls.begin(), tomls.end(), t) == tomls.end()) tomls.push_back(t);
    }
    const bool own_shaders = Exists(folder + "/shaders/" + id + ".xsh");
    if (tomls.empty() && own_shaders) tomls.push_back(folder + "/UserData/achievements/" + id + ".toml");
    for (const std::string& t : tomls) {
      if (t.find("/achievements/") == std::string::npos) continue;
      for (const auto& e : EarnedInToml(t)) earned.push_back(e);
      port.sources.push_back(t);
      if (!named) {
        // (its name and icon: the port's own app)
        const std::vector<uint8_t> param = ReadAll(folder + "/sce_sys/param.json");
        port.folder = folder;
        port.name = TitleName(std::string(param.begin(), param.end()));
        port.icon = ReadAll(folder + "/sce_sys/icon0.png");
        named = true;
      }
      found = true;
    }
    // an Xbox 360 emulator's profiles (saves/XUID/FFFE07D1/00010000/XUID/:
    // the profile's FFFE07D1.gpd and a TITLEID.gpd per game, made when it's
    // first played): their achievements only (the emulator isn't the game)
    FindFiles(folder + "/saves", "FFFE07D1.gpd", 5, profiles);
    for (const std::string& profile : profiles) {
      const std::string g = profile.substr(0, profile.rfind('/') + 1) + id + ".gpd";
      for (const auto& e : EarnedInGpd(g)) earned.push_back(e);
      port.sources.push_back(g);
      found = true;
    }
  }
  closedir(dir);
  // each achievement once, at the earliest it was earned
  std::sort(earned.begin(), earned.end());
  for (const auto& e : earned) {
    if (port.earned.empty() || port.earned.back().first != e.first) port.earned.push_back(e);
  }
  return found;
}

Result Run(const std::string& set_id, int32_t user, const std::vector<std::pair<uint32_t, int64_t>>& earned,
           size_t achievement_count) {
  Result result;
  const std::string folder = kJobFolder;
  {
    std::ofstream out(folder + "/unlocked.txt", std::ios::trunc);
    int64_t last = 0;
    for (const auto& [id, when] : earned) {
      out << id << ' ' << when << '\n';
      last = std::max(last, when);
    }
    if (achievement_count && earned.size() >= achievement_count) out << 0 << ' ' << last << '\n';
  }
  const std::string job_id = std::to_string(uint32_t(std::time(nullptr)));
  std::remove((folder + "/result.txt").c_str());
  {
    std::ofstream out(folder + "/job.txt", std::ios::trunc);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", unsigned(user));
    out << job_id << ' ' << set_id << ' ' << hex << '\n';
  }
  if (!SendRelay()) {
    result.loader = false;
    result.error = "The payload loader (port 9021: etaHEN, elfldr...) didn't answer.";
    return result;
  }
  const auto sent_at = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - sent_at < std::chrono::seconds(20)) {
    std::ifstream in(folder + "/result.txt");
    std::string id, set, status;
    if (in >> id >> set >> status >> result.set_state >> result.progress_state && id == job_id) {
      result.ok = status == "OK";
      if (!result.ok) {
        result.error = result.progress_state == "no-template"
                           ? "The profile has no PS5 game with trophies yet: any PS5 game with trophies has to "
                             "be played once first."
                           : "The console didn't take the trophies.";
      }
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  result.error = "The relay didn't answer.";
  return result;
}

bool Watch(const std::string& set_id, int32_t user, const std::string& name,
           const std::vector<std::pair<uint32_t, uint32_t>>& trophies, const std::vector<int>& grades,
           const std::vector<std::string>& names, const std::vector<std::string>& sources) {
  const std::string folder = std::string(kWatchFolder) + "/" + set_id;
  mkdir("/data/trophies360", 0777);
  mkdir(kWatchFolder, 0777);
  mkdir(folder.c_str(), 0777);
  {
    std::ofstream out(folder + "/game.txt", std::ios::trunc);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", unsigned(user));
    out << set_id << ' ' << hex << '\n' << name << '\n';
  }
  {
    // "ACHIEVEMENT TROPHY GRADE NAME"
    std::ofstream out(folder + "/map.txt", std::ios::trunc);
    for (size_t i = 0; i < trophies.size(); ++i) {
      out << trophies[i].first << ' ' << trophies[i].second << ' ' << grades[i] << ' ' << names[i] << '\n';
    }
  }
  std::ofstream out(folder + "/sources.txt", std::ios::trunc);
  for (const std::string& source : sources) out << source << '\n';
  return bool(out);
}

bool WatcherAlive() {
  // (its heartbeat: the time it wrote last)
  std::ifstream in(std::string(kWatchFolder) + "/alive");
  long long at = 0;
  return in >> at && std::llabs(int64_t(std::time(nullptr)) - at) < 15;
}

bool StartWatcher() {
  if (WatcherAlive()) return true;
  mkdir("/data/trophies360", 0777);
  mkdir(kWatchFolder, 0777);
  std::vector<uint8_t> elf;
  {
    std::lock_guard<std::mutex> lock(payloads_mutex);
    elf = watcher_elf;
  }
  return SendPayload(elf);
}

bool AddToAutoload() {
  constexpr char kManager[] = "/data/pldmgr";
  struct stat st;
  if (stat(kManager, &st) != 0 || !S_ISDIR(st.st_mode)) return false;  // (no Payload Manager)
  const std::string folder = std::string(kManager) + "/payloads/local/recomptrophy-watcher";
  const std::string elf = folder + "/RecompTrophy-Watcher.elf";
  mkdir((std::string(kManager) + "/payloads").c_str(), 0777);
  mkdir((std::string(kManager) + "/payloads/local").c_str(), 0777);
  mkdir(folder.c_str(), 0777);
  // [ps5 port] the watcher, when there's none (RecompTrophy's own, maybe newer,
  // is left alone)
  if (!Exists(elf)) {
    std::vector<uint8_t> watcher;
    {
      std::lock_guard<std::mutex> lock(payloads_mutex);
      watcher = watcher_elf;
    }
    if (watcher.empty()) return false;
    std::ofstream out(elf + ".tmp", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(watcher.data()), std::streamsize(watcher.size()));
    out.close();
    if (!out || std::rename((elf + ".tmp").c_str(), elf.c_str()) != 0) return false;
  }
  if (!Exists(folder + "/meta.json")) {
    char when[40];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S+0000", std::gmtime(&now));
    std::ofstream(folder + "/meta.json", std::ios::trunc)
        << "{\n  \"schema\": 2,\n  \"kind\": \"local\",\n  \"filename\": \"RecompTrophy-Watcher.elf\",\n"
        << "  \"installed_at\": \"" << when << "\",\n  \"origin\": \"web_upload\",\n  \"origin_detail\": \"\"\n}\n";
  }
  return true;
}

bool InAutoloadList() {
  std::ifstream in("/data/pldmgr/autoload.txt");
  std::string line;
  while (std::getline(in, line)) {
    line.erase(line.find_last_not_of("\r\n") + 1);
    if (line == "local/recomptrophy-watcher") return true;
  }
  return false;
}

bool SendRelayAlone() {
  // its line in the autoload list (root's only): by the relay, with no job
  std::remove((std::string(kJobFolder) + "/job.txt").c_str());
  return SendRelay();
}

}  // namespace recomptrophy::install

// [ps5 port] see native_trophies.h. Uses RecompTrophy by lamarking
// (https://github.com/cptlamarca/RecompTrophy), GPL-3.0, used with permission.
#include "native_trophies.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__PROSPERO__)
#include <sys/mount.h>
extern "C" int _fstatfs(int fd, struct statfs* buf);
extern "C" int sceUserServiceGetInitialUser(int32_t* user_id);
extern "C" int sceUserServiceGetForegroundUser(int32_t* user_id);
extern "C" int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/frame_stats.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include "game.h"
#include "install.h"
#include "trophyset.h"

// The hub's PS5 Trophies (Game page).
REXCVAR_DEFINE_BOOL(ps5_trophies, true, "PS5",
                    "Put the game's trophies in the PS5's own Trophies list and show the PS5's trophy pop-ups "
                    "(needs a payload loader on port 9021)");

namespace gta4ps5 {

namespace fs = std::filesystem;
namespace rt = recomptrophy;

namespace {

// Raised when what's made here changes: the set is made and installed again.
constexpr int kVersion = 1;
constexpr char kApp[] = "/app0";
constexpr char kDoneFile[] = "/data/trophies360/done.txt";  // (RecompTrophy's: the sets made)

using Bytes = std::vector<uint8_t>;
using Earned = std::vector<std::pair<uint32_t, int64_t>>;

struct Job {
  rt::game::Info info;
  int32_t user = -1;
  fs::path own;          // this feature's files (/app0/UserData/LibertyRecomp/ps5_trophies)
  std::string toml_rel;  // the game's achievements file, inside the app's folder
};

// ------------------------------------------------------------------ files

Bytes ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadText(const fs::path& path) {
  const Bytes b = ReadFile(path);
  std::string s(b.begin(), b.end());
  s.erase(s.find_last_not_of(" \t\r\n") + 1);
  return s;
}

// Written beside, then renamed over.
bool WriteText(const fs::path& path, const std::string& text) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(text.data(), std::streamsize(text.size()))) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

bool CopyFile(const fs::path& from, const fs::path& to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!in || !out) return false;
  out << in.rdbuf();
  out.close();
  return bool(out);
}

bool Exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

// ---------------------------------------------------- the game's achievements

// The fields RecompTrophy's game::Read takes from default.xex's XDBF, from the
// running game's (the same XDBF): English and Portuguese (English when none).
bool ReadAchievements(rex::system::KernelState* ks, rt::game::Info& info) {
  using rex::system::XLanguage;
  using rex::system::util::XdbfSection;
  info.title_id = ks->title_id();
  const auto db = ks->title_xdbf();
  if (!db.is_valid()) return false;
  const bool english = bool(db.GetEntry(XdbfSection::kStringTable, uint64_t(XLanguage::kEnglish)));
  const bool portuguese = bool(db.GetEntry(XdbfSection::kStringTable, uint64_t(XLanguage::kPortuguese)));
  auto en = [&](uint16_t id) {
    return english      ? db.GetStringTableEntry(XLanguage::kEnglish, id)
           : portuguese ? db.GetStringTableEntry(XLanguage::kPortuguese, id)
                        : std::string();
  };
  auto pt = [&](uint16_t id) { return portuguese ? db.GetStringTableEntry(XLanguage::kPortuguese, id) : en(id); };
  info.title = en(0x8000);
  if (const auto icon = db.icon()) info.icon.assign(icon.buffer, icon.buffer + icon.size);
  for (const auto& e : db.GetAchievements()) {
    rt::game::Achievement a;
    a.id = uint16_t(e.id);
    if (a.id < 1 || a.id > 127) continue;
    a.name[0] = en(e.label_id), a.detail[0] = en(e.description_id);
    a.name[1] = pt(e.label_id), a.detail[1] = pt(e.description_id);
    if (a.name[1].empty()) a.name[1] = a.name[0], a.detail[1] = a.detail[0];
    a.gamerscore = uint16_t(e.gamerscore);
    a.hidden = !(uint32_t(e.flags) & 8);
    if (const auto picture = db.GetEntry(XdbfSection::kImage, uint32_t(e.image_id)))
      a.picture.assign(picture.buffer, picture.buffer + picture.size);
    info.achievements.push_back(std::move(a));
  }
  if (info.title.empty()) info.title = "Xbox 360";
  return !info.achievements.empty();
}

int32_t CurrentUser() {
  int32_t user = -1;
#if defined(__PROSPERO__)
  if (sceUserServiceGetInitialUser(&user) != 0 || user < 0) sceUserServiceGetForegroundUser(&user);
#endif
  return user;
}

// What the set is made from (a change makes it again).
std::string Fingerprint(const rt::game::Info& info, const std::string& set, const std::string& name,
                        const Bytes& icon) {
  uint64_t h = 14695981039346656037ull;
  auto add = [&](const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  };
  auto number = [&](uint64_t v) { add(&v, sizeof(v)); };
  auto text = [&](const std::string& s) { number(s.size()), add(s.data(), s.size()); };
  auto bytes = [&](const Bytes& b) { number(b.size()), add(b.data(), b.size()); };
  number(uint64_t(kVersion)), number(info.title_id), text(set), text(name), bytes(icon), bytes(info.icon);
  for (const auto& a : info.achievements) {
    number(a.id), number(a.gamerscore), number(a.hidden ? 1 : 0);
    for (int l = 0; l < 2; ++l) text(a.name[l]), text(a.detail[l]);
    bytes(a.picture);
  }
  char out[20];
  std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return out;
}

// --------------------------------------------- what the console was given

bool IsDone(const std::string& set) {
  std::ifstream in(kDoneFile);
  std::string id;
  while (in >> id)
    if (id == set) return true;
  return false;
}

void MarkDone(const std::string& set) {  // (as RecompTrophy marks a set made)
  if (IsDone(set)) return;
  std::ofstream(kDoneFile, std::ios::app) << set << '\n';
}

// state.txt: "VERSION SET FINGERPRINT USER" and the achievements brought in.
struct State {
  int version = 0;
  std::string set, fingerprint;
  int32_t user = -1;
  std::set<uint32_t> earned;
};

State ReadState(const fs::path& file) {
  State s;
  std::ifstream in(file);
  std::string user;
  if (!(in >> s.version >> s.set >> s.fingerprint >> user)) return {};
  s.user = int32_t(std::strtoul(user.c_str(), nullptr, 16));
  uint32_t id = 0;
  while (in >> id) s.earned.insert(id);
  return s;
}

bool WriteState(const fs::path& file, const State& s) {
  char user[16];
  std::snprintf(user, sizeof(user), "%x", unsigned(s.user));
  std::string text = std::to_string(s.version) + ' ' + s.set + ' ' + s.fingerprint + ' ' + user + '\n';
  for (const uint32_t id : s.earned) text += std::to_string(id) + ' ';
  return WriteText(file, text + '\n');
}

// --------------------------------------------------------- the app's folder

// Where the console keeps this app (/data/homebrew/PPSA94004...), which the
// game sees as /app0: a note written here is looked for in each app folder.
std::string RealAppFolder(const fs::path& own) {
  const fs::path note = own / "here.txt";
  const std::string rel = note.lexically_relative(kApp).generic_string();
  if (rel.empty() || rel.rfind("..", 0) == 0) return {};
  std::mt19937_64 random(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                         uint64_t(std::time(nullptr)) ^ uint64_t(getpid()));
  char token[24];
  std::snprintf(token, sizeof(token), "%016llx", static_cast<unsigned long long>(random()));
  if (!WriteText(note, token)) return {};
  std::vector<std::string> places;
  auto add = [&](std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p.size() > 1 && p[0] == '/' && std::find(places.begin(), places.end(), p) == places.end())
      places.push_back(p);
  };
  auto add_folders_in = [&](const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') add(dir + "/" + e->d_name);
    closedir(d);
  };
  // (as RecompTrophy names the ports first)
  add_folders_in("/data/homebrew");
#if defined(__PROSPERO__)
  // where /app0 is mounted from
  const int fd = ::open((std::string(kApp) + "/eboot.bin").c_str(), O_RDONLY);
  if (fd >= 0) {
    struct statfs st {};
    if (_fstatfs(fd, &st) == 0 && std::string_view(st.f_fstypename) == "nullfs") add(st.f_mntfromname);
    ::close(fd);
  }
#endif
  for (const char* kind : {"ext", "usb"})
    for (int i = 0; i < 8; ++i) add_folders_in("/mnt/" + std::string(kind) + std::to_string(i) + "/homebrew");
  for (const std::string& p : places)
    if (ReadText(p + "/" + rel) == token) return p;
  return {};
}

// ------------------------------------------------------------------ the work

int64_t MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

void Work(const Job& job) {
  const auto started = std::chrono::steady_clock::now();
  const rt::game::Info& info = job.info;
  const std::string set = rt::trophyset::SetId(info.title_id);
  const int32_t user = job.user;
  if (user < 0) {
    REXLOG_WARN("ps5 trophies: no user signed in; skipped");
    return;
  }
  const fs::path payloads = fs::path(kApp) / "payloads";
  Bytes relay = ReadFile(payloads / "recomptrophy-relay.elf");
  Bytes watcher = ReadFile(payloads / "recomptrophy-watcher.elf");
  if (relay.empty() || watcher.empty()) {
    REXLOG_WARN("ps5 trophies: the app has no payloads/recomptrophy-*.elf; skipped");
    return;
  }
  rt::install::SetPayloads(std::move(relay), std::move(watcher));
  std::error_code ec;
  fs::create_directories(job.own, ec);

  // The game among the console's apps, as RecompTrophy finds it (its name,
  // icon, the achievements earned and the files they're in), and this app's
  // achievements file by its real path, watched for before it's made.
  rt::install::Port port;
  const bool has_port = rt::install::FindPort(info.title_id, port);
  const std::string real = RealAppFolder(job.own);
  std::vector<std::string> sources = port.sources;
  if (!real.empty()) {
    const std::string toml = real + "/" + job.toml_rel;
    if (std::find(sources.begin(), sources.end(), toml) == sources.end()) sources.push_back(toml);
  }
  // (and this app's earned ones through /app0, wherever the app is; each once, the earliest)
  Earned all = port.earned;
  for (const auto& e : rt::install::EarnedInToml(std::string(kApp) + "/" + job.toml_rel)) all.push_back(e);
  std::sort(all.begin(), all.end());
  Earned earned;
  for (const auto& e : all)
    if (rt::trophyset::TrophyId(info, e.first) && (earned.empty() || earned.back().first != e.first))
      earned.push_back(e);
  const Bytes own_param = ReadFile(fs::path(kApp) / "sce_sys" / "param.json");
  std::string name = has_port && !port.name.empty() ? port.name
                                                     : rt::install::TitleName(std::string(own_param.begin(), own_param.end()));
  if (name.empty()) name = info.title;
  const Bytes icon = has_port && !port.icon.empty() ? port.icon : ReadFile(fs::path(kApp) / "sce_sys" / "icon0.png");
  const std::string fingerprint = Fingerprint(info, set, name, icon);
  REXLOG_INFO("ps5 trophies: {} ({:08X}), set {}, user {:x}: {} achievements, {} earned; app folder {}; {} file(s) "
              "to watch",
              name, info.title_id, set, unsigned(user), info.achievements.size(), earned.size(),
              real.empty() ? "not found" : real, sources.size());

  // Installed when it wasn't, when it changed, or when there are earned ones
  // the console wasn't given yet (RecompTrophy's done.txt, and this
  // feature's state.txt). The relay itself leaves a set already there alone.
  const fs::path state_file = job.own / "state.txt";
  const State last = ReadState(state_file);
  const bool done = IsDone(set);
  const bool same = last.version == kVersion && last.set == set && last.fingerprint == fingerprint && last.user == user;
  size_t fresh = 0;
  for (const auto& e : earned) fresh += last.earned.count(e.first) ? 0 : 1;
  bool relay_sent = false;
  if (!done || !same || fresh) {
    REXLOG_INFO("ps5 trophies: installing ({})", !done              ? "not on the PS5 yet"
                                                 : last.version == 0 ? "first time from the game"
                                                 : !same             ? "out of date"
                                                                     : "earned achievements to bring in");
    // the set, made again only when what it's made from changed
    const fs::path ucp = job.own / "TROPHY.UCP", list = job.own / "trophies.txt", built = job.own / "built.txt";
    if (ReadText(built) != fingerprint || !fs::exists(ucp, ec) || !fs::exists(list, ec)) {
      const auto t = std::chrono::steady_clock::now();
      fs::remove(built, ec);
      if (!rt::trophyset::Build(info, set, name, icon, ucp.string(), list.string(), nullptr)) {
        REXLOG_WARN("ps5 trophies: the set couldn't be written to {}", job.own.string());
        return;
      }
      WriteText(built, fingerprint);
      REXLOG_INFO("ps5 trophies: set made ({} trophies, {} KB) in {} ms", info.achievements.size() + 1,
                  fs::file_size(ucp, ec) >> 10, MsSince(t));
    }
    const std::string job_folder = rt::install::kJobFolder;
    mkdir("/data/trophies360", 0777);
    mkdir(job_folder.c_str(), 0777);
    if (!CopyFile(ucp, job_folder + "/TROPHY.UCP") || !CopyFile(list, job_folder + "/trophies.txt")) {
      REXLOG_WARN("ps5 trophies: {} can't be written; skipped", job_folder);
      return;
    }
    // (the watcher beside the Payload Manager first: the relay adds its autoload line)
    rt::install::AddToAutoload();
    Earned trophies;
    for (const auto& [achievement, when] : earned) trophies.emplace_back(rt::trophyset::TrophyId(info, achievement), when);
    const rt::install::Result result = rt::install::Run(set, user, trophies, info.achievements.size());
    if (!result.loader) {
      REXLOG_INFO("ps5 trophies: no payload loader answered on port 9021 (etaHEN, elfldr...); skipped");
      return;
    }
    relay_sent = true;
    if (!result.ok) {
      REXLOG_WARN("ps5 trophies: not installed: {} (set {}, progress {})", result.error,
                  result.set_state.empty() ? "?" : result.set_state,
                  result.progress_state.empty() ? "?" : result.progress_state);
      return;
    }
    MarkDone(set);
    State now{kVersion, set, fingerprint, user, {}};
    for (const auto& e : earned) now.earned.insert(e.first);
    if (!WriteState(state_file, now)) REXLOG_WARN("ps5 trophies: {} can't be written", state_file.string());
    REXLOG_INFO("ps5 trophies: installed (set {}, progress {}), {} earned brought in", result.set_state,
                result.progress_state, trophies.size());
  } else {
    REXLOG_INFO("ps5 trophies: on the PS5 and up to date");
  }

  // The PS5's own pop-ups: RecompTrophy's watcher, for this game's files.
  if (set == "NPWR99990_00" || Exists("/data/trophies360/native/" + set)) {
    REXLOG_INFO("ps5 trophies: /data/trophies360/native/{} is there: no pop-ups from the watcher", set);
    return;
  }
  if (sources.empty()) {
    REXLOG_WARN("ps5 trophies: the game's achievements file wasn't found on the console: no pop-ups");
    return;
  }
  std::vector<std::pair<uint32_t, uint32_t>> map;
  std::vector<int> grades;
  std::vector<std::string> names;
  for (const auto& a : info.achievements) {
    map.emplace_back(a.id, rt::trophyset::TrophyId(info, a.id));
    grades.push_back(a.gamerscore <= 10 ? 4 : a.gamerscore <= 25 ? 3 : 2);
    names.push_back(a.name[0]);
  }
  if (!rt::install::Watch(set, user, name, map, grades, names, sources))
    REXLOG_WARN("ps5 trophies: {}/{} can't be written", rt::install::kWatchFolder, set);
  if (Exists(std::string(rt::install::kWatchFolder) + "/" + set + "/off"))
    REXLOG_INFO("ps5 trophies: this game's pop-ups are turned off in RecompTrophy");
  const bool manager = rt::install::AddToAutoload();
  if (rt::install::WatcherAlive()) {
    // (its autoload line, when the Payload Manager's list doesn't show it)
    if (manager && !relay_sent && !rt::install::InAutoloadList()) rt::install::SendRelayAlone();
    REXLOG_INFO("ps5 trophies: the watcher runs already{}; done in {} ms",
                manager ? "" : " (no Payload Manager: not loaded at the console's start)", MsSince(started));
  } else if (rt::install::StartWatcher()) {
    std::this_thread::sleep_for(std::chrono::seconds(4));
    REXLOG_INFO("ps5 trophies: watcher sent, {}{}; done in {} ms", rt::install::WatcherAlive() ? "running" : "not running yet",
                manager ? "" : " (no Payload Manager: not loaded at the console's start)", MsSince(started));
  } else {
    REXLOG_INFO("ps5 trophies: no payload loader answered on port 9021 for the watcher; skipped");
  }
}

void Worker(Job job) {
#if defined(__PROSPERO__)
  // (with the game's other threads, off the renderer's own cores)
  scePthreadSetaffinity(pthread_self(), rex::frame_stats::kOtherThreadCpus);
#endif
  try {
    Work(job);
  } catch (const std::exception& e) {
    REXLOG_WARN("ps5 trophies: stopped: {}", e.what());
  } catch (...) {
    REXLOG_WARN("ps5 trophies: stopped by an error");
  }
}

}  // namespace

void StartNativeTrophies(rex::Runtime* runtime, const fs::path& user_data) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  try {
    if (!REXCVAR_GET(ps5_trophies)) {
      REXLOG_INFO("ps5 trophies: off (the hub's PS5 Trophies)");
      return;
    }
    auto* ks = runtime ? runtime->kernel_state() : nullptr;
    if (!ks) return;
    // (copied here: the worker never touches the runtime)
    Job job;
    if (!ReadAchievements(ks, job.info)) {
      REXLOG_WARN("ps5 trophies: the game's achievements couldn't be read; skipped");
      return;
    }
    job.user = CurrentUser();
    job.own = rex::filesystem::GetUserFolder() / "LibertyRecomp" / "ps5_trophies";
    // (where the runtime keeps them: kernel_state.cpp)
    const fs::path toml = user_data / "achievements" / fmt::format("{:08X}.toml", job.info.title_id);
    job.toml_rel = toml.lexically_normal().lexically_relative(kApp).generic_string();
    if (job.toml_rel.empty() || job.toml_rel.rfind("..", 0) == 0) {
      REXLOG_WARN("ps5 trophies: {} isn't in the app's folder; skipped", toml.string());
      return;
    }
    std::thread(Worker, std::move(job)).detach();
  } catch (const std::exception& e) {
    REXLOG_WARN("ps5 trophies: not started: {}", e.what());
  } catch (...) {
    REXLOG_WARN("ps5 trophies: not started");
  }
}

}  // namespace gta4ps5

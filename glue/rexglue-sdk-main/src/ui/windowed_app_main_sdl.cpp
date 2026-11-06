/**
 * @file        ui/windowed_app_main_sdl.cpp
 * @brief       Entry point for windowed applications (SDL3 windowing)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>
#if REX_PLATFORM_PS5
#include <SDL3/SDL_hints.h>
#endif

REXCVAR_DEFINE_BOOL(diagnostics, false, "Diagnostics",
                    "Enable diagnostic logging and instrumentation");
REXCVAR_DEFINE_STRING(diagnostics_categories, "", "Diagnostics",
                      "Comma-separated diagnostic categories (empty = all; logging controls "
                      "general application logs independently of diagnostic artifacts)");

#if REX_PLATFORM_ANDROID
#include <android/log.h>
#endif

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

#if REX_PLATFORM_PS5
void Ps5EarlyFailure(const char* what);  // (below)
#endif

int RunWindowedApp(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  // The diagnostics policy is command-line-owned and must be installed before
  // any logger sinks or worker threads exist. The REXLOG macros and the
  // native-probe instrumentation are gated on it; without this call the
  // policy stays unconfigured and every log line is suppressed.
  {
    bool diagnostics = REXCVAR_GET(diagnostics);
    std::string categories = REXCVAR_GET(diagnostics_categories);
#if REX_PLATFORM_PS5
    // [ps5 port] No command line on the PS5: the log is on unless the hub's
    // Save Logs setting is off (read straight from its config file, as the
    // policy has to be set before the config is loaded).
    // (The renderer's own profiler is not offered: it turns off the
    // renderer's command coalescing while it runs and the PS5 drops to a few
    // frames a second. The sampling profiler in rex/ps5_diag.h replaces it.)
    if (!diagnostics) {
      bool save_logs = true;
      if (std::FILE* f = std::fopen("/app0/UserData/LibertyRecomp/native.toml", "rb")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
          const char* start = line + std::strspn(line, " \t");
          const char* key = std::strstr(line, "ps5_save_logs");
          if (key && key == start && std::strstr(key, "false")) save_logs = false;
        }
        std::fclose(f);
      }
      if (save_logs) diagnostics = true, categories = "logging";
    }
#endif
    std::string diagnostics_error;
    if (!rex::diagnostics::Configure(diagnostics, categories, &diagnostics_error)) {
      std::fprintf(stderr, "diagnostics: %s\n", diagnostics_error.c_str());
#if REX_PLATFORM_ANDROID
      // stderr is discarded on Android; without this the app just closes.
      __android_log_print(ANDROID_LOG_ERROR, "LibertyRecomp", "diagnostics: %s",
                          diagnostics_error.c_str());
#endif
      return EXIT_FAILURE;
    }
  }
  rex::InitLoggingEarly();

  int result;
  {
#if REX_PLATFORM_PS5
    // [ps5 port] SDL has no PS5 video driver: its dummy driver keeps windows and
    // events working; the picture goes to the TV through VK_KHR_display.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
#endif
    rex::ui::SDLWindowedAppContext app_context;
    if (!app_context.Initialize()) {
#if REX_PLATFORM_PS5
      Ps5EarlyFailure("SDL didn't start");
#endif
      return EXIT_FAILURE;
    }

#if REX_PLATFORM_WIN32
    // Apartment-threaded COM for shell dialogs.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }
#endif

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    result = app->OnInitialize() ? app_context.RunMainMessageLoop() : EXIT_FAILURE;
#if REX_PLATFORM_PS5
    // [ps5 port] Skip the teardown: guest threads can't be stopped reliably.
    rex::FlushLogging();
    std::_Exit(result);
#endif

    app->InvokeOnDestroy();
  }

#if REX_PLATFORM_WIN32
  CoUninitialize();
#endif

  return result;
}

#if REX_PLATFORM_WIN32
// Convert wide argv from CommandLineToArgvW to UTF-8 for cvar::Init.
std::vector<std::string> WideArgsToUtf8(int argc, wchar_t** wargv) {
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    std::wstring wide(wargv[i]);
    if (wide.empty()) {
      args.emplace_back();
      continue;
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr,
                                   0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    args.push_back(std::move(utf8));
  }
  return args;
}
#endif

}  // namespace

#if REX_PLATFORM_WIN32

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE hinstance_prev, LPWSTR command_line,
                    int show_cmd) {
  (void)hinstance;
  (void)hinstance_prev;
  (void)command_line;
  (void)show_cmd;

  int wargc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  auto utf8_args = WideArgsToUtf8(wargc, wargv);
  LocalFree(wargv);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(utf8_args.size());
  for (auto& s : utf8_args) {
    argv_ptrs.push_back(s.data());
  }
  return RunWindowedApp(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

#else

#if REX_PLATFORM_ANDROID
// Renames main to SDL_main, which SDLActivity resolves in libmain.so.
#include <SDL3/SDL_main.h>
#endif

#if REX_PLATFORM_PS5

// [ps5 port] Work in the app's folder (logs/, UserData/, Saves/ go there; the
// process starts elsewhere, often read-only), and show a notification so the
// player can tell the app started at all.
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
extern "C" int sceKernelSendNotificationRequest(int, void*, size_t, int);
extern "C" int sceSystemServiceLoadExec(const char* path, char* const argv[]);
static void Ps5Notify(const char* text) {
  struct {
    char unused[45];
    char message[3075];
  } request;
  std::memset(&request, 0, sizeof(request));
  std::strncpy(request.message, text, sizeof(request.message) - 1);
  sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

#include <SDL3/SDL_error.h>
namespace {
void Ps5EarlyFailure(const char* what) {
  char text[600];
  std::snprintf(text, sizeof(text), "%s: %s", what, SDL_GetError());
  Ps5Notify(text);
  // (the log file doesn't exist yet: the reason in logs/startup_error.log)
  mkdir("/app0/logs", 0777);
  if (FILE* f = std::fopen("/app0/logs/startup_error.log", "w")) {
    std::fprintf(f, "%s\n", text);
    std::fclose(f);
  }
}
}  // namespace

// An uncaught C++ exception ends in abort() with its message on stderr, which
// nobody sees on a PS5: put it in the log and on screen first.
#include <exception>
#include <typeinfo>
static void Ps5Terminate() {
  char text[600] = "Uncaught exception";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(text, sizeof(text), "Uncaught %s: %s", typeid(e).name(), e.what());
    } catch (...) {
    }
  }
  REXLOG_ERROR("{}", text);
  Ps5Notify(text);
  sleep(2);  // (let the notification show)
  std::abort();
}

// Every 10 s: the progress counters (rex/ps5_diag.h), so a black screen's
// log shows which stage stopped moving.
#include <rex/chrono/clock.h>
#include <rex/frame_stats.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <rex/ps5_diag.h>
#include <time.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
REXCVAR_DEFINE_BOOL(ps5_cpu_sampler, true, "PS5",
                    "Sample where each thread spends its time (100 times a second) and log the "
                    "busiest code every 30 s");
// The sampling profiler's clock: from 30 s in, every 10 ms signal each thread
// once (rex/ps5_diag.h), and every 30 s log what they were doing.
static void Ps5Profiler() {
  std::this_thread::sleep_for(std::chrono::seconds(30));
  auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (REXCVAR_GET(ps5_cpu_sampler)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rex::ps5diag::ProfileTick();
    if (std::chrono::steady_clock::now() >= report_at) {
      rex::ps5diag::ProfileReport();
      report_at += std::chrono::seconds(30);
    }
  }
}
REXCVAR_DEFINE_BOOL(ps5_hang_reports, true, "PS5",
                    "When the game stops drawing for a few seconds, log where every thread is");
// A stall in play (no new frame for 300 ms after the game has been running):
// where every thread is while it lasts, at most once every 20 s.
static void Ps5HitchWatch() {
  using namespace rex::frame_stats;
  int64_t last_sample_ns = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // A thread crashed: the crash hook ends the game; if it hangs (flushing
    // the log behind a lock the crashed thread holds), this does. Nothing
    // else here then: logging could wait on that thread's heap lock.
    if (const uint64_t crashed = rex::ps5diag::crashed_at_ms.load(std::memory_order_acquire)) {
      if (rex::ps5diag::NowMs() - crashed > 5000) _exit(1);
      continue;
    }
    const uint64_t frames = g_presents.load(std::memory_order_acquire);
    const int64_t last = g_last_present_ns.load(std::memory_order_acquire), now = NowNs();
    // (again every 20 s while the same frame stays up: a freeze's report
    // must reach the end of the log, where a sent log is cut)
    if (frames < 900 || !last || now - last < 300000000 || now - last_sample_ns < 20000000000ll ||
        !REXCVAR_GET(ps5_hang_reports))
      continue;
    last_sample_ns = now;
    REXLOG_INFO("ps5 hitch: no new frame for {} ms after frame {}; where the threads are:",
                (now - last) / 1000000, frames);
    rex::ps5diag::SampleAllThreads();
  }
}
static void Ps5DiagMonitor() {
  using namespace rex::ps5diag;
  for (int tick = 1;; ++tick) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    if (tick == 1) {
      timespec res{};
      clock_getres(CLOCK_MONOTONIC, &res);
      REXLOG_INFO("ps5 diag: clock resolution {} ns, host tick frequency {} Hz", res.tv_nsec,
                  rex::chrono::Clock::QueryHostTickFrequency());
      // The CPUs this process may run on (the game's threads share them).
      cpuset_t mask;
      CPU_ZERO(&mask);
      const int rc = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(mask), &mask);
      std::string cpus;
      for (int c = 0; c < 64; ++c)
        if (CPU_ISSET(c, &mask)) cpus += (cpus.empty() ? "" : ",") + std::to_string(c);
      REXLOG_INFO("ps5 diag: {} CPUs online, this process runs on [{}] ({}), page size {}",
                  sysconf(_SC_NPROCESSORS_ONLN), cpus, rc, getpagesize());
    }
    // Game time against real time: the guest clock's seconds over these 10,
    // the game's own step for its last frame, and the frames shown.
    {
      static uint64_t last_guest = 0, last_presents = 0;
      static int64_t last_ns = 0;
      const uint64_t guest = rex::chrono::Clock::QueryGuestTickCount();
      const uint64_t presents = rex::frame_stats::g_presents.load();
      static uint64_t last_shown = 0;
      const uint64_t shown = rex::frame_stats::g_display_presents.load();
      const int64_t now_ns = rex::frame_stats::NowNs();
      float step = 0;
      if (auto* kernel = rex::system::kernel_state()) {
        constexpr uint32_t kGameplayTimeStep = 0x82C6C2AC;
        const uint32_t bits =
            rex::memory::load_and_swap<uint32_t>(kernel->memory()->TranslateVirtual(kGameplayTimeStep));
        std::memcpy(&step, &bits, sizeof(step));
      }
      using namespace rex::frame_stats;
      const uint64_t rec_us = g_recorder_us.exchange(0), rec_n = g_recorder_frames.exchange(0);
      const uint64_t rec_max = g_recorder_max_us.exchange(0), wait_us = g_handoff_wait_us.exchange(0);
      const uint64_t gpu_wait_us = g_gpu_wait_us.exchange(0);
      const uint64_t image_wait_us = g_output_image_wait_us.exchange(0);
      const uint64_t record_commands_us = g_record_commands_us.exchange(0);
      const uint64_t output_callback_us = g_output_callback_us.exchange(0);
      const uint64_t slot_wait_us = g_title_slot_wait_us.exchange(0), pacer_us = g_pacer_wait_us.exchange(0);
      const uint64_t guest_wait_us = uint64_t(double(g_title_guest_wait_ticks.exchange(0)) * 1e6 /
                                              double(rex::chrono::Clock::QueryHostTickFrequency()));
      const uint64_t main_wait_us = uint64_t(double(g_main_guest_wait_ticks.exchange(0)) * 1e6 /
                                             double(rex::chrono::Clock::QueryHostTickFrequency()));
      if (last_ns) {
        const double real = double(now_ns - last_ns) / 1e9;
        const double guest_s = double(guest - last_guest) / double(rex::chrono::Clock::guest_tick_frequency());
        const uint64_t frames = presents - last_presents;
        auto per_frame = [frames](uint64_t us) { return frames ? double(us) / 1000.0 / double(frames) : 0.0; };
        const double frame_ms = frames ? real * 1000.0 / double(frames) : 0.0;
        // The game's own work per frame is its frame time less its waits for
        // the renderer; the recorder's is its time less the frame limit's hold.
        REXLOG_INFO("ps5 timing: real {:.2f}s, guest clock {:.2f}s, game step {:.1f} ms, frame {:.1f} ms "
                    "(TV {:.1f} fps); "
                    "game {:.1f} ms of work (+{:.1f} waiting for the renderer, {:.1f} waiting on other "
                    "threads), game's main thread {:.1f} ms of work (+{:.1f} waiting), recorder {:.1f} ms (worst {:.1f}, "
                    "waiting for the GPU {:.1f}, for its output image {:.1f}; recording {:.1f}, the rest "
                    "of the output step {:.1f}), held for the frame limit "
                    "{:.1f} ms, worker waiting on the recorder {:.1f} ms",
                    real, guest_s, step * 1000.0, frame_ms, double(shown - last_shown) / real,
                    std::max(0.0, frame_ms - per_frame(slot_wait_us) - per_frame(guest_wait_us) -
                                      per_frame(pacer_us)),
                    per_frame(slot_wait_us), per_frame(guest_wait_us),
                    std::max(0.0, frame_ms - per_frame(main_wait_us)), per_frame(main_wait_us),
                    rec_n ? double(rec_us) / 1000.0 / double(rec_n) : 0.0, double(rec_max) / 1000.0,
                    rec_n ? double(gpu_wait_us) / 1000.0 / double(rec_n) : 0.0,
                    rec_n ? double(image_wait_us) / 1000.0 / double(rec_n) : 0.0,
                    rec_n ? double(record_commands_us) / 1000.0 / double(rec_n) : 0.0,
                    rec_n ? double(output_callback_us > record_commands_us ? output_callback_us - record_commands_us : 0) /
                                1000.0 / double(rec_n)
                          : 0.0,
                    per_frame(pacer_us),
                    per_frame(wait_us));
        if (g_frame_generation_active.load()) {
          const uint64_t pairs = g_fg_pairs.exchange(0), alone = g_fg_unpaired.exchange(0);
          // (why scenes went out alone, by the first gate each failed)
          std::string dropped;
          for (uint32_t i = 0; i < uint32_t(FgDrop::kCount); ++i)
            if (const uint32_t n = g_fg_drop[i].exchange(0))
              dropped += fmt::format("{}{} {}", dropped.empty() ? "" : ", ", n, kFgDropNames[i]);
          REXLOG_INFO("ps5 frame generation: {:.1f} pairs a second (generated + real), {:.1f} real frames "
                      "alone; each image at least {} vblank{}; {} pairs made; alone because: {}; {} skips in "
                      "the game's frame numbers",
                      double(pairs) / real, double(alone) / real, g_fg_vblanks_per_image.load(),
                      g_fg_vblanks_per_image.load() == 1 ? "" : "s", g_fg_published.exchange(0),
                      dropped.empty() ? "nothing" : dropped, g_fg_sequence_gaps.exchange(0));
        }
        const uint64_t kept = g_renderer_cpu_mask.load();
        const char* layout = kept == kOwnCoresLayout        ? "own cores, recycling near"
                             : kept == kOwnCoresApartLayout ? "own cores, recycling anywhere"
                             : kept == kFirstClusterCpus    ? "together"
                                                            : "anywhere";
        const bool lean = g_lean_capture.load();
        const bool prefetch = g_capture_prefetch.load();
        const bool main_core = IsOwnCoresLayout(kept) && g_main_core.load();
        const bool on_core = g_capture_on_core.load();
        const uint64_t ring_wait_us = g_capture_ring_wait_us.exchange(0);
        REXLOG_INFO("ps5 cpus: game {:#x}, worker {:#x}, recorder {:#x}, capture {:#x} ({}{}{}; {} capture{}{}{}; "
                    "the game waited {:.1f} ms on the capture thread)",
                    g_cpus_title.exchange(0), g_cpus_worker.exchange(0), g_cpus_recorder.exchange(0),
                    g_cpus_capture.exchange(0), layout, main_core ? ", main thread on its own core" : "",
                    g_renderer_cpu_compare.load() ? ", comparing" : "",
                    lean ? "lean" : "classic", on_core ? " on its own core" : "",
                    lean && !prefetch ? " without prefetch" : "",
                    g_capture_core_compare.load() ? ", comparing" : "", double(ring_wait_us) / 1000.0);
        // Game Thread Capture on Compare: own core and lean in turn, 10 s each.
        if (g_capture_core_compare.load()) g_capture_on_core.store(!on_core);
        // Renderer Threads on Compare: the other layout for the next 10 s.
        uint64_t next = kept;
        if (g_renderer_cpu_compare.load()) {
          next = kept == kOwnCoresLayout ? kOwnCoresApartLayout : kOwnCoresLayout;
          g_renderer_cpu_mask.store(next);
        }
        // The game's other threads leave the renderer's cores (or get them
        // back), every 10 s so threads started since are moved too (a thread
        // already on its CPUs is left alone).
        static bool others_moved = false;
        if (IsOwnCoresLayout(next) || others_moved) {
          others_moved = IsOwnCoresLayout(next);
          rex::ps5diag::KeepOtherThreadsOn(RendererRoleMask(next, RendererRole::kOther),
                                           g_title_thread.load(), g_game_main_thread.load(),
                                           RendererRoleMask(next, RendererRole::kMain));
        }
      }
      last_guest = guest;
      last_presents = presents;
      last_shown = shown;
      last_ns = now_ns;
    }
    REXLOG_INFO(
        "ps5 diag {}s: faults {} (mmio {}, watch {}, unhandled {}), vblanks {}, interrupts {}, "
        "ring writes {}, ring batches {}",
        tick * 10, counters[kAccessViolations].load(), counters[kMmioAccesses].load(),
        counters[kWatchFaults].load(), counters[kUnhandledFaults].load(), counters[kVblanks].load(),
        counters[kInterrupts].load(), counters[kWritePointer].load(),
        counters[kPrimaryBuffers].load());
    // The game stopped presenting frames (the native renderer doesn't use the
    // GPU ring, so frames are what show it): guest threads stuck in one kernel
    // wait for over 5 s, and where every thread is. In play, threads waiting
    // on idle job queues are normal and not listed.
    static uint64_t last_presents_seen = 0, stuck_ticks = 0, samples = 0;
    const uint64_t presents_now = rex::frame_stats::g_presents.load();
    stuck_ticks = presents_now == last_presents_seen ? stuck_ticks + 1 : 0;
    last_presents_seen = presents_now;
    const uint64_t now = NowMs();
    for (WaitSlot& w : wait_slots) {
      if (!stuck_ticks) break;
      uint64_t since = w.since_ms.load(std::memory_order_acquire);
      if (since && since < now && now - since > 5000) {
        REXLOG_INFO("ps5 diag:   guest thread {:X} waiting {}s on {} {} handle {:08X}{} from game code {:08X}",
                    w.thread_id, (now - since) / 1000, w.count > 1 ? w.count : 1,
                    ObjectTypeName(w.object_type), w.handle, w.count > 1 ? " (first of several)" : "",
                    w.guest_lr);
        // its callers: each frame's back chain at 0(r1), the caller's LR
        // saved at -8 from the caller's frame (mflr r12; stw r12,-8(r1);
        // stwu ...) - read only where the guest memory is committed
        if (auto* kernel = rex::system::kernel_state(); kernel && w.guest_sp && w.guest_stack_lo) {
          auto* memory = kernel->memory();
          auto readable = [&](uint32_t a) {
            uint32_t protect = 0;
            auto* heap = memory->LookupHeap(a);
            return heap && heap->QueryProtect(a, &protect) && (protect & rex::memory::kMemoryProtectRead);
          };
          std::string callers;
          uint32_t sp = w.guest_sp;
          for (int i = 0; i < 8; ++i) {
            if (sp < w.guest_stack_lo || sp + 4 > w.guest_stack_hi || (sp & 3) || !readable(sp)) break;
            const uint32_t back = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
            if (back <= sp || back < w.guest_stack_lo + 8 || back > w.guest_stack_hi || !readable(back - 8)) break;
            callers += fmt::format(" < {:08X}",
                                   rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(back - 8)));
            sp = back;
          }
          if (!callers.empty()) REXLOG_INFO("ps5 diag:     called from{}", callers);
        }
        if (const auto* cs = static_cast<const uint8_t*>(w.critical_section)) {
          auto be32 = [cs](int off) {
            return uint32_t(cs[off]) << 24 | uint32_t(cs[off + 1]) << 16 |
                   uint32_t(cs[off + 2]) << 8 | uint32_t(cs[off + 3]);
          };
          auto le32 = [cs](int off) {
            return uint32_t(cs[off]) | uint32_t(cs[off + 1]) << 8 | uint32_t(cs[off + 2]) << 16 |
                   uint32_t(cs[off + 3]) << 24;
          };
          std::string raw;
          for (int b = 0; b < 28; ++b) raw += fmt::format("{:02X}{}", cs[b], b % 4 == 3 ? " " : "");
          REXLOG_INFO("ps5 diag:     critical section {:08X} entered from {:08X}: lock count {}, recursion {}, "
                      "held by kthread {:08X}; initialized {}; bytes {}",
                      w.critical_section_guest, w.critical_section_lr, int32_t(le32(0x10)),
                      int32_t(be32(0x14)), be32(0x18),
                      CriticalSectionInitBy(w.critical_section_guest)
                          ? fmt::format("by game code {:08X}", CriticalSectionInitBy(w.critical_section_guest))
                          : std::string("never (by RtlInitializeCriticalSection)"),
                      raw);
        }
      }
    }
    if ((stuck_ticks == 3 || stuck_ticks == 6) && samples < 2 && REXCVAR_GET(ps5_hang_reports)) {
      ++samples;
      SampleAllThreads();
    }
  }
}

int main(int argc, char* argv[]) {
  chdir("/app0");
  rex::ps5diag::RegisterThread("main");
  // A thread that crashed (it would retry its faulting instruction forever,
  // behind a frozen loading screen): say so and close the game.
  // (It runs in the fault handler, maybe inside the allocator: no logging,
  // which allocates. The crash line is already at the end of the log file.)
  rex::ps5diag::crash_hook = [](const char*) {
    Ps5Notify("The game crashed - send /app0/logs to the developer");
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    sleep(2);
    _exit(1);
  };
  // EXIT in the game's main menu: start the app again, into its hub.
  rex::ps5diag::title_exit_hook = [] {
    REXLOG_INFO("back to the hub (restarting the app)");
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    char* args[] = {nullptr};
    const int rc = sceSystemServiceLoadExec("/app0/eboot.bin", args);
    REXLOG_ERROR("restarting failed ({:#x}) - closing", uint32_t(rc));
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });
    _exit(0);
  };
  std::set_terminate(Ps5Terminate);
  std::thread(Ps5DiagMonitor).detach();
  std::thread(Ps5Profiler).detach();
  std::thread(Ps5HitchWatch).detach();
  int result = RunWindowedApp(argc, argv);
  if (result != 0) {
    Ps5Notify("Stopped with an error - see /app0/logs");
    sleep(2);  // (let the notification show)
  }
  // Leave without the C runtime's exit handlers (with the game's threads
  // still around, they end in a crash report).
  _exit(result);
}

#else

int main(int argc, char* argv[]) {
  return RunWindowedApp(argc, argv);
}
#endif  // REX_PLATFORM_PS5

#endif

// [ps5 port] The guest's presented frames, for the PS5 build's frame rate
// counter and frame time capture: the renderer notes each present.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

#include <pthread.h>

#include <rex/chrono/clock.h>

namespace rex::frame_stats {

inline std::atomic<uint64_t> g_presents{0};
// Images sent to the TV (more than the game's frames with frame generation).
inline std::atomic<uint64_t> g_display_presents{0};
inline std::atomic<int64_t> g_last_present_ns{0};
inline std::atomic<uint32_t> g_last_frame_us{0};
// The last kHistory frames' times (frame n at n % kHistory), for the graph.
inline constexpr uint32_t kHistory = 256;
inline std::atomic<uint32_t> g_history_us[kHistory];
// [ps5 port] What the TV showed (ps5_display's pacing thread, from VideoOut's
// flips): how long each frame stayed on screen (frame n at n % kTvHistory),
// how many it showed, and frames that came and went between two looks. The
// FPS counter reads these: a frame held three vblanks is a stutter there even
// when the game finished it on time.
inline constexpr uint32_t kTvHistory = 512;
inline std::atomic<uint32_t> g_tv_hold_us[kTvHistory];
inline std::atomic<uint64_t> g_tv_shown{0};
inline std::atomic<uint64_t> g_tv_missed{0};

// [ps5 port] Set while FSR 3 frame generation is chosen and the device can
// run it; with it off (or unavailable) presentation is decoupled.
inline std::atomic<bool> g_frame_generation_active{false};
// [ps5 port] Frame generation's cadence: how many vblanks each image (generated
// or real) stays on screen. 1: the game at half the TV's rate (30 -> 60 at
// 60 Hz, 60 -> 120 at 120 Hz); 2 at 120 Hz while the game can't hold 60 (30 ->
// 60). Set by the game's frame limiter; the display follows it through the
// hook, which re-applies VideoOut's flip rate.
inline std::atomic<uint32_t> g_fg_vblanks_per_image{1};
inline std::atomic<void (*)(const char*)> g_display_pacing_changed{nullptr};
// [ps5 port] Called by the presenter right before it queues an image to the
// TV (ps5_display.cpp: holds it until the image before has had its full time).
inline std::atomic<void (*)()> g_before_present{nullptr};
// Whether the image about to be presented carries a new game frame (not the
// one on the TV again: the presenter re-presents it at the TV's rate).
inline std::atomic<bool> g_present_new_frame{true};
// UI-thread repaints of the frame already on the TV that were skipped.
inline std::atomic<uint64_t> g_repeat_paints_skipped{0};
// [ps5 port] Asks the presenter for one paint of the frame on the TV (set by
// the presenter), and how many were made: ps5_display.cpp's refresh while the
// game is behind.
inline std::atomic<void (*)()> g_request_keepalive_paint{nullptr};
inline std::atomic<uint64_t> g_keepalive_paints{0};
// The frame limit in effect: the setting's, or 40 while Smooth Heavy Areas
// holds a 60 limit there (the game's frame limiter sets it; 0 until then).
inline std::atomic<uint32_t> g_effective_frame_limit{0};
// Images presented as generated + real pairs, and real frames that went out
// alone (no generated partner), for the timing line.
inline std::atomic<uint64_t> g_fg_pairs{0}, g_fg_unpaired{0};
// [ps5 port] Why a scene went out without a generated partner, counted for the
// 10 s frame generation line (the first gate each scene failed), and the
// pairs handed to the presenter. Names: kFgDropNames.
enum class FgDrop : uint32_t {
  kNoScene,          // the frame had no 3D scene (menus, loading, the phone)
  kSceneSize,        // the scene's size didn't match the temporal setup
  kAssets,           // the temporal images couldn't be made
  kSpatialMotion,    // a draw's motion couldn't be followed: FSR fell back to spatial
  kSpatialWorld,     // no world draws seen
  kSpatialDepth,     // no depth
  kSpatialCamera,    // the camera wasn't taken up
  kSpatialTime,      // no time passed since the last scene
  kSpatialEncode,    // the upscaler refused the frame
  kSecondComposite,  // a second final composite in one frame
  kEncodeGate,       // motion, camera or presenter state at the generation step
  kPresenter,        // the presenter can't show generated frames
  kAudit,            // the motion audit failed
  kConfigure,        // frame generation couldn't be set up
  kCamera,           // the camera matrices don't invert
  kEncodeRefused,    // frame generation refused the inputs (its status is logged)
  kHistoryReset,     // FSR reset its history: a real frame only
  kHudNotExact,      // a HUD draw couldn't be replayed onto the pair
  kDisplayStale,     // the display image wasn't this frame's
  kConvert,          // converting the pair for the TV failed
  kPublishRefused,   // the presenter wouldn't take the pair
  kCount
};
inline constexpr const char* kFgDropNames[] = {
    "no scene", "scene size", "assets", "spatial: motion", "spatial: no world", "spatial: depth",
    "spatial: camera", "spatial: time", "spatial: upscaler", "second composite", "generation gate",
    "presenter", "audit", "configure", "camera", "refused", "history reset", "HUD not exact",
    "display stale", "convert", "publish refused"};
inline std::atomic<uint32_t> g_fg_drop[uint32_t(FgDrop::kCount)];
inline std::atomic<uint64_t> g_fg_published{0}, g_fg_sequence_gaps{0};
inline void NoteFgDrop(FgDrop why) {
  if (g_frame_generation_active.load(std::memory_order_relaxed))
    g_fg_drop[uint32_t(why)].fetch_add(1, std::memory_order_relaxed);
}

inline int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Renderer stage times, summed until the PS5's 10 s timing line reads and
// clears them: the recorder's time per frame (recording and submitting the
// Vulkan commands) and the command worker's wait for the recorder.
inline std::atomic<uint64_t> g_recorder_us{0}, g_recorder_frames{0}, g_recorder_max_us{0};
inline std::atomic<uint64_t> g_handoff_wait_us{0};
// Time the renderer spent waiting for the graphics card to finish an earlier
// frame (a frame slot to reuse): high means the GPU is the limit.
inline std::atomic<uint64_t> g_gpu_wait_us{0};
inline void NoteRecorderFrame(uint64_t us) {
  g_recorder_us.fetch_add(us, std::memory_order_relaxed);
  g_recorder_frames.fetch_add(1, std::memory_order_relaxed);
  uint64_t max = g_recorder_max_us.load(std::memory_order_relaxed);
  while (us > max && !g_recorder_max_us.compare_exchange_weak(max, us, std::memory_order_relaxed)) {
  }
}

// Where a frame's time went, for the 10 s timing line and the hitch log.
// The title (game) thread's waits for a free frame slot (the renderer behind
// it), summed for the timing line and for its current frame.
inline std::atomic<uint64_t> g_title_slot_wait_us{0}, g_title_frame_slot_wait_us{0};
inline std::atomic<uint32_t> g_title_last_slot_wait_us{0};
// The game's render thread (set at its first present) and the time it spent
// blocked on the game's own other threads: guest waits, sleeps and lock
// spins. Its frame less these and the renderer waits is its work.
inline std::atomic<uintptr_t> g_title_thread{0};
inline std::atomic<uint64_t> g_title_guest_wait_ticks{0};
// The same, for the current frame only (the hitch line takes and clears it).
inline std::atomic<uint64_t> g_title_frame_guest_wait_ticks{0};
// The game's main thread (its entry point's, which runs the game's update:
// set when it starts) and its time in the same waits, for the timing line.
inline std::atomic<uintptr_t> g_game_main_thread{0};
inline std::atomic<uint64_t> g_main_guest_wait_ticks{0};
inline bool IsTitleThread() {
  const uintptr_t title = g_title_thread.load(std::memory_order_relaxed);
  return title && title == reinterpret_cast<uintptr_t>(pthread_self());
}
class TitleWaitScope {
 public:
  TitleWaitScope() {
    const uintptr_t self = reinterpret_cast<uintptr_t>(pthread_self());
    if (self == g_title_thread.load(std::memory_order_relaxed)) {
      main_ = false;
    } else if (self == g_game_main_thread.load(std::memory_order_relaxed)) {
      main_ = true;
    } else {
      return;
    }
    begin_ = rex::chrono::Clock::QueryHostTickCount();
  }
  ~TitleWaitScope() {
    if (!begin_) return;
    const uint64_t ticks = rex::chrono::Clock::QueryHostTickCount() - begin_;
    if (main_) {
      g_main_guest_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
      return;
    }
    g_title_guest_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
    g_title_frame_guest_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
  }
  TitleWaitScope(const TitleWaitScope&) = delete;
  TitleWaitScope& operator=(const TitleWaitScope&) = delete;

 private:
  uint64_t begin_ = 0;
  bool main_ = false;
};

// [ps5 port] The CPUs the renderer's threads ran on since the last timing
// line (a bit per CPU), and the CPUs they are kept on (0: anywhere).
inline std::atomic<uint32_t> g_cpus_title{0}, g_cpus_worker{0}, g_cpus_recorder{0};
inline std::atomic<uint64_t> g_renderer_cpu_mask{0};
// The CPUs a game thread may use (13 of the 16), and the first cluster's 8.
inline constexpr uint64_t kGameCpus = 0x1FFF, kFirstClusterCpus = 0xFF;
// Renderer Threads on Own Cores (a layout, not a mask): the game's render
// thread, the recorder and the worker each get a core of their own (CPUs 0,
// 2 and 4; a core's two hardware threads are numbered side by side), and
// every other thread runs on CPUs 5-12, so CPUs 1 and 3 stay empty (sharing
// the render thread's core with the recycling thread measured slower). The
// thread that recycles finished commands writes the commands and reference
// counts the render thread fills next: kept on CPUs 5-7, the same half of
// the CPU (and the same L3 cache) as the renderer's three, those come over
// within that half instead of from the other one. "Apart" leaves it on 5-12.
// g_main_core gives the game's main thread a core of its own as well (CPU 6,
// 7 left empty, the rest on 5 and 8-12). Off: 0.6b2 turned it on and off
// every 10 s under Compare, and both of the first sessions froze a few
// seconds after one of those moves.
inline constexpr uint64_t kOwnCoresLayout = uint64_t(1) << 63;
inline constexpr uint64_t kOwnCoresApartLayout = uint64_t(1) << 62;
inline constexpr uint64_t kTitleCpu = 1u << 0, kRecorderCpu = 1u << 2, kWorkerCpu = 1u << 4,
                          kRetirerCpus = 0xE0, kMainCpu = 1u << 6, kMainCoreCpus = 0xC0;
inline constexpr uint64_t kOtherThreadCpus = kGameCpus & ~uint64_t(0x1F);
// The main thread's own core, under Own Cores (off; see above).
inline std::atomic<bool> g_main_core{false};
inline bool IsOwnCoresLayout(uint64_t layout) {
  return layout == kOwnCoresLayout || layout == kOwnCoresApartLayout;
}
enum class RendererRole { kTitle, kWorker, kRecorder, kRetirer, kMain, kOther };
// The CPUs one thread is kept on under a layout (0: anywhere).
inline uint64_t RendererRoleMask(uint64_t layout, RendererRole role) {
  if (IsOwnCoresLayout(layout)) {
    const uint64_t main_core = g_main_core.load(std::memory_order_relaxed) ? kMainCoreCpus : 0;
    switch (role) {
      case RendererRole::kTitle: return kTitleCpu;
      case RendererRole::kRecorder: return kRecorderCpu;
      case RendererRole::kWorker: return kWorkerCpu;
      case RendererRole::kRetirer:
        return (layout == kOwnCoresLayout ? kRetirerCpus : kOtherThreadCpus) & ~main_core;
      case RendererRole::kMain:
        if (main_core) return kMainCpu;
        return kOtherThreadCpus;
      default: return kOtherThreadCpus & ~main_core;
    }
  }
  if ((role == RendererRole::kOther || role == RendererRole::kMain) && layout != kFirstClusterCpus)
    return kGameCpus;
  return layout ? layout : kGameCpus;
}
// Renderer Threads on Compare: the timing report switches between own cores
// with the recycling thread on the renderer's half of the CPU and anywhere
// every 10 s, so one session measures both.
inline std::atomic<bool> g_renderer_cpu_compare{false};
// Game Thread Capture (ps5_lean_capture): lean (each resource held once for
// two frames, with prefetching) or classic. On Compare it stays lean and the
// timing report switches lean's prefetching off and on every 10 s; the game's
// render thread picks the change up at its next frame.
inline std::atomic<bool> g_lean_capture{true};
inline std::atomic<bool> g_lean_capture_compare{false};
inline std::atomic<bool> g_capture_prefetch{true};
// The layout the Renderer Threads setting asks for at start.
inline uint64_t RendererCpuMaskFor(std::string_view setting) {
  if (setting == "any") return 0;
  if (setting == "together") return kFirstClusterCpus;
  if (setting == "apart") return kOwnCoresApartLayout;
  return kOwnCoresLayout;  // "cores" and "compare"
}
#if defined(__PROSPERO__)
extern "C" int32_t sceKernelGetCurrentCpu(void);
extern "C" int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);
inline void NoteCpu(std::atomic<uint32_t>& cpus) {
  const int32_t cpu = sceKernelGetCurrentCpu();
  if (cpu >= 0 && cpu < 32) cpus.fetch_or(1u << cpu, std::memory_order_relaxed);
}
// The game's render thread, the worker and the recorder hand each other
// every draw; kept on one of the CPU's two clusters, they share its cache.
inline void KeepRendererThreadOnItsCpus() {
  scePthreadSetaffinity(pthread_self(), RendererRoleMask(g_renderer_cpu_mask.load(std::memory_order_relaxed),
                                                         RendererRole::kRetirer));
}
// Once a frame on each renderer thread: applies its CPUs when the layout changed.
inline void FollowRendererCpuMask(uint64_t& applied, RendererRole role) {
  const uint64_t mask = g_renderer_cpu_mask.load(std::memory_order_relaxed);
  if (mask == applied) return;
  applied = mask;
  scePthreadSetaffinity(pthread_self(), RendererRoleMask(mask, role));
}
#else
inline void NoteCpu(std::atomic<uint32_t>&) {}
inline void KeepRendererThreadOnItsCpus() {}
inline void FollowRendererCpuMask(uint64_t&, RendererRole) {}
#endif

inline void NoteTitleSlotWait(uint64_t us) {
  g_title_slot_wait_us.fetch_add(us, std::memory_order_relaxed);
  g_title_frame_slot_wait_us.fetch_add(us, std::memory_order_relaxed);
}
// The recorder holding a finished frame back for the frame limit (time that
// is not work), summed and for its current frame; and its last frame's work.
inline std::atomic<uint64_t> g_pacer_wait_us{0}, g_frame_pacer_us{0};
// The recorder waiting for its output image to be free (the presenter still
// using it: with frame generation, for the generated frame).
inline std::atomic<uint64_t> g_output_image_wait_us{0};
// The recorder's frame: recording the commands, and the whole output step
// around it (with frame generation and FSR 3 encoding and submission).
inline std::atomic<uint64_t> g_record_commands_us{0}, g_output_callback_us{0};
inline std::atomic<uint32_t> g_last_recorder_busy_us{0};
// Small draws into the sun shadow map dropped before they reach the renderer
// (Small Object Shadows off), for the capture line.
inline std::atomic<uint64_t> g_title_small_shadows_dropped{0};
// Environment reflection draws, clears and resolves left out on the frames
// between its redraws (ps5_environment_reflection_every).
inline std::atomic<uint64_t> g_title_reflection_commands_skipped{0};

// [ps5 port] The game frame (its guest output publication) each image sent to
// the TV carried, by the image's number from 1: VideoOut numbers its flips the
// same way (RADV's flip argument), so the pacing log (ps5_display.cpp) can tell
// a new frame on the screen from a repeat of the last one.
inline constexpr uint32_t kImageHistory = 1024;
inline std::atomic<uint64_t> g_images_sent{0};
inline std::atomic<uint64_t> g_image_frame[kImageHistory];
// From the presenter, for each image it queued to the TV.
inline void NoteImageSent(uint64_t frame) {
  const uint64_t n = g_images_sent.load(std::memory_order_relaxed) + 1;
  g_image_frame[n % kImageHistory].store(frame, std::memory_order_relaxed);
  g_images_sent.store(n, std::memory_order_release);
}

// From the renderer, once per guest frame.
inline void NotePresent() {
  const int64_t now = NowNs();
  const int64_t last = g_last_present_ns.exchange(now, std::memory_order_acq_rel);
  const uint32_t us = last ? uint32_t((now - last) / 1000) : 0;
  if (last) g_last_frame_us.store(us, std::memory_order_relaxed);
  g_title_last_slot_wait_us.store(uint32_t(g_title_frame_slot_wait_us.exchange(0, std::memory_order_relaxed)),
                                  std::memory_order_relaxed);
  const uint64_t n = g_presents.load(std::memory_order_relaxed);
  g_history_us[n % kHistory].store(us, std::memory_order_relaxed);
  g_presents.store(n + 1, std::memory_order_release);
}

}  // namespace rex::frame_stats

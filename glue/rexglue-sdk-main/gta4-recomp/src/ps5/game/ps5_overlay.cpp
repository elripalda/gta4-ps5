// GTA IV PS5 - the frame rate counter, the frame time capture (see
// ps5_overlay.h) and the hand-off of an accepted Return to Hub request.
#include "ps5_overlay.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/frame_stats.h>
#include <rex/logging.h>

#include "../hub/ps5_hub.h"
#include "../lifecycle/f02_menu_request.h"
#include "ps5_display.h"

REXCVAR_DEFINE_STRING(ps5_fps_counter, "false", "PS5",
                      "The frame rate counter at the left edge of the screen while playing: false (off), simple "
                      "(frames a second and the frame time) or true (detailed: also the slowest frames, a graph, "
                      "the resolution, the TV's refresh rate, the frame limit and frame generation)");
REXCVAR_DECLARE(uint32_t, gta4_frame_limit);
REXCVAR_DEFINE_BOOL(ps5_frametime_capture, false, "PS5",
                    "Record every frame's time to a CSV file in /app0/logs");
REXCVAR_DEFINE_BOOL(ps5_overlay_every_vblank, false, "PS5",
                    "Repaint the game's overlay every vblank (the old way: each new frame waits "
                    "behind repeats of the last one)");

namespace gta4ps5 {

namespace f02 = gta4::ps5::lifecycle::f02;

const char* BuildVersion() { return GTA4PS5_VERSION; }

GameOverlay::GameOverlay(rex::ui::ImGuiDrawer* drawer,
                         std::function<void()> return_to_hub,
                         std::function<void(bool)> set_title_input_captured)
    : ImGuiDialog(drawer),
      return_to_hub_(std::move(return_to_hub)),
      set_title_input_(std::move(set_title_input_captured)) {
  window_start_ns_ = rex::frame_stats::NowNs();
  window_frames_ = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  if (REXCVAR_GET(ps5_frametime_capture)) {
    std::error_code ec;
    std::filesystem::create_directories("/app0/logs", ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char name[96];
    std::strftime(name, sizeof(name), "/app0/logs/frametimes-%Y-%m-%d-%H-%M-%S.csv", &tm);
    csv_ = std::fopen(name, "w");
    if (csv_) {
      std::fputs("frame,seconds,frame_ms\n", csv_);
      REXLOG_INFO("PS5: frame time capture to {}", name);
    } else {
      REXLOG_WARN("PS5: couldn't start the frame time capture ({})", name);
    }
  }
}

GameOverlay::~GameOverlay() {
  if (csv_) std::fclose(csv_);
}

void GameOverlay::SetTitleInputCaptured(bool captured) {
  if (captured == title_input_captured_) return;
  title_input_captured_ = captured;
  if (set_title_input_) set_title_input_(captured);
}

void GameOverlay::ResetReturnPrompt() {
  f02::ResetReturnRequest();
  // The guest-side confirmation controller closes the message box and restores
  // the pause state on its own thread.
  f02::RequestConfirmationReset();
  return_failed_ = false;
  SetTitleInputCaptured(false);
}

void GameOverlay::ShowReturnFailure() {
  return_failed_ = true;
  f02::ResetReturnRequest();
  // The title was asked to terminate; do not let the pad drive a half-closed
  // session.
  SetTitleInputCaptured(true);
}

void GameOverlay::UpdateReturnRequest() {
  if (return_failed_) return;
  // The guest's native confirmation posts exactly one request on accept. The
  // guest pause frame is frozen from that point on, so there is nothing to
  // arbitrate here.
  if (!f02::ConsumeReturnRequest()) return;
  REXLOG_INFO("F02: return to hub accepted in the native pause-menu dialog");
  SetTitleInputCaptured(true);
  if (return_to_hub_) return_to_hub_();
}

void GameOverlay::DrawReturnFailure(ImGuiIO& io) {
  if (!return_failed_) return;
  const ImVec2 size = io.DisplaySize;
  const float s = std::min(size.x / 1920.0f, size.y / 1080.0f);
  const float sw = size.x / s, sh = size.y / s;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const auto& fonts = hub::Fonts();
  ImFont* bold = fonts.body_bold ? fonts.body_bold : ImGui::GetFont();
  ImFont* body = fonts.body ? fonts.body : ImGui::GetFont();
  auto P = [&](float x, float y) { return ImVec2(x * s, y * s); };
  auto Text = [&](ImFont* font, float pixels, float x, float y, ImU32 color, const char* text) {
    dl->AddText(font, pixels * s, P(x, y), color, text);
  };

  dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 170));
  const float pw = 760, ph = 220;
  const float px = (sw - pw) * 0.5f, py = (sh - ph) * 0.5f;
  dl->AddRectFilled(P(px, py), P(px + pw, py + ph), IM_COL32(16, 16, 18, 245), 14 * s);
  dl->AddRect(P(px, py), P(px + pw, py + ph), IM_COL32(255, 255, 255, 45), 14 * s, 0, 1.5f * s);
  Text(bold, 34, px + 40, py + 34, IM_COL32(244, 245, 247, 255), "RETURN TO HUB");
  Text(body, 22, px + 40, py + 96, IM_COL32(220, 222, 226, 230),
       "This session could not be closed safely.");
  Text(body, 16, px + 40, py + 132, IM_COL32(200, 202, 208, 160),
       "Close the game from the PS5's home screen.");
}

void GameOverlay::Sample() {
  using namespace rex::frame_stats;
  const int64_t now = NowNs();
  const uint64_t frames = g_presents.load(std::memory_order_acquire);
  const uint64_t tv = g_tv_shown.load(std::memory_order_acquire);
  const int64_t span = now - window_start_ns_;
  if (span < 500'000'000) return;
  const uint64_t n = frames - window_frames_;
  const uint64_t shown = tv - window_tv_;
  game_fps_ = float(double(n) * 1e9 / double(span));
  // [ps5 port] What the TV showed when the pacing thread sees it: the game can
  // finish 60 frames a second while some stay up for two refreshes and others
  // never reach the screen, which the game's own count reads as a clean 60.
  from_tv_ = shown > 0;
  const uint64_t counted = from_tv_ ? shown : n;
  fps_ = float(double(counted) * 1e9 / double(span));
  ms_ = counted ? float(double(span) / 1e6 / double(counted)) : 0.0f;
  window_start_ns_ = now;
  window_frames_ = frames;
  window_tv_ = tv;
  // the slowest frames of the last few seconds: 1% low and the worst one
  std::vector<uint32_t> times;
  if (from_tv_) {
    const uint32_t count = uint32_t(std::min<uint64_t>(tv, 300));
    times.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
      if (const uint32_t us = g_tv_hold_us[(tv - 1 - i) % kTvHistory].load(std::memory_order_relaxed))
        times.push_back(us);
  } else {
    const uint32_t count = uint32_t(std::min<uint64_t>(frames > 1 ? frames - 1 : 0, kHistory));
    times.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
      if (const uint32_t us = g_history_us[(frames - 1 - i) % kHistory].load(std::memory_order_relaxed))
        times.push_back(us);
  }
  // a stutter: a frame on screen half again as long as the usual one
  stutters_ = 0;
  if (from_tv_ && times.size() >= 30) {
    std::vector<uint32_t> sorted = times;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const uint32_t usual = sorted[sorted.size() / 2];
    for (const uint32_t us : times) stutters_ += us > usual + usual / 2 && us > usual + 5000;
  }
  if (times.empty()) {
    low_fps_ = worst_ms_ = 0;
    return;
  }
  std::sort(times.begin(), times.end());
  const uint32_t p99 = times[size_t(double(times.size() - 1) * 0.99)];
  low_fps_ = p99 ? float(1e6 / double(p99)) : 0.0f;
  worst_ms_ = float(times.back()) / 1000.0f;
}

void GameOverlay::Capture() {
  if (!csv_) return;
  const uint64_t frames = rex::frame_stats::g_presents.load(std::memory_order_acquire);
  if (frames == captured_) return;
  // (one line for the newest frame; frames between two draws of the overlay
  // share its time, which is rare at the rates the game runs)
  const int64_t now = rex::frame_stats::g_last_present_ns.load(std::memory_order_acquire);
  if (!capture_start_ns_) capture_start_ns_ = now;
  const double ms = rex::frame_stats::g_last_frame_us.load(std::memory_order_relaxed) / 1000.0;
  for (uint64_t f = captured_ + 1; f <= frames; ++f)
    std::fprintf(csv_, "%llu,%.4f,%.3f\n", (unsigned long long)f, double(now - capture_start_ns_) / 1e9, ms);
  captured_ = frames;
  if ((frames & 255) == 0) std::fflush(csv_);
}

namespace {
// green at 60 or better, white at 30, then yellow, then red
ImU32 RateColor(float ms) {
  return ms <= 17.5f ? IM_COL32(120, 230, 140, 255)
         : ms <= 34.5f ? IM_COL32(244, 245, 247, 255)
         : ms <= 50.5f ? IM_COL32(245, 200, 80, 255)
                       : IM_COL32(240, 90, 80, 255);
}
}  // namespace

bool GameOverlay::WantsContinuousRepaint() const { return REXCVAR_GET(ps5_overlay_every_vblank); }

void GameOverlay::OnDraw(ImGuiIO& io) {
  Capture();
  UpdateReturnRequest();
  DrawReturnFailure(io);
  // (the overlay stays up with the counter off, drawing nothing: the game's
  // frames reach the TV the same way either way)
  const std::string mode = REXCVAR_GET(ps5_fps_counter);
  if (mode.empty() || mode == "false" || mode == "off") return;
  const bool simple = mode == "simple";
  Sample();
  const ImVec2 size = io.DisplaySize;
  const float s = std::min(size.x / 1920.0f, size.y / 1080.0f);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const auto& fonts = hub::Fonts();
  ImFont* bold = fonts.body_bold ? fonts.body_bold : ImGui::GetFont();
  ImFont* body = fonts.body ? fonts.body : ImGui::GetFont();
  auto P = [&](float x, float y) { return ImVec2(x * s, y * s); };

  // ---- the counter, on the left edge halfway down (the game's own HUD uses
  // the corners). Simple: frames a second and the frame time. Detailed: also
  // the slowest frames, what the picture is set to and a graph.
  std::vector<std::string> info;
  if (!simple) {
    // what the picture is: the game's resolution, the TV, the frame limit
    if (!resolution_at_ns_ || rex::frame_stats::NowNs() - resolution_at_ns_ > 1'000'000'000) {
      resolution_ = rex::cvar::GetFlagByName("resolution");
      resolution_at_ns_ = rex::frame_stats::NowNs();
    }
    std::string res = resolution_;
    if (res == "4k") res = "4K";
    std::string tv = display::HighRefresh() ? (display::Vrr() ? "120 Hz VRR" : "120 Hz") : "60 Hz";
    const bool fg = rex::frame_stats::g_frame_generation_active.load(std::memory_order_relaxed);
    info.push_back(res + "  \xC2\xB7  " + tv +
                   (fg ? std::string() : "  \xC2\xB7  limit " + std::to_string(REXCVAR_GET(gta4_frame_limit))));
    if (fg) {
      const uint32_t v = std::max<uint32_t>(1, rex::frame_stats::g_fg_vblanks_per_image.load(std::memory_order_relaxed));
      const int out = (display::HighRefresh() ? 120 : 60) / int(v);
      info.push_back("Frame generation " + std::to_string(out / 2) + " \xE2\x86\x92 " + std::to_string(out));
    }
    // the renderer's share of the last frame (the graphics thread's recording)
    if (from_tv_) info.push_back(fmt::format("Game {:.0f} FPS  \xC2\xB7  {} stutter{}", game_fps_, stutters_,
                                              stutters_ == 1 ? "" : "s"));
    if (const uint32_t rec = rex::frame_stats::g_last_recorder_busy_us.load(std::memory_order_relaxed))
      info.push_back("Renderer " + fmt::format("{:.1f}", rec / 1000.0) + " ms a frame");
  }
  const float w = simple ? 196 : 264, pad = 12, line_h = 19;
  const float graph_h = simple ? 0 : 48;
  const float h = simple ? pad + 36 + pad - 4
                         : pad + 36 + 20 + float(info.size()) * line_h + 10 + graph_h + pad;
  const float x = 12, y = (size.y / s - h) * 0.5f;
  dl->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32(0, 0, 0, 170), 8 * s);
  char big[16], unit[] = "FPS", ms[32], small[96];
  if (fps_ > 0) {
    std::snprintf(big, sizeof(big), "%.0f", fps_);
    std::snprintf(ms, sizeof(ms), "%.1f ms", ms_);
    std::snprintf(small, sizeof(small), "1%% low %.0f  \xC2\xB7  worst frame %.0f ms", low_fps_, worst_ms_);
  } else {
    std::snprintf(big, sizeof(big), "--");
    ms[0] = small[0] = 0;
  }
  const ImU32 col = fps_ > 0 ? RateColor(ms_) : IM_COL32(244, 245, 247, 255);
  const float big_size = 36 * s;
  const ImVec2 bsz = bold->CalcTextSizeA(big_size, 1e9f, 0, big);
  dl->AddText(bold, big_size, P(x + pad, y + pad - 5), col, big);
  dl->AddText(bold, 15 * s, ImVec2((x + pad) * s + bsz.x + 6 * s, (y + pad + 13) * s), IM_COL32(255, 255, 255, 170), unit);
  const ImVec2 msz = bold->CalcTextSizeA(20 * s, 1e9f, 0, ms);
  dl->AddText(bold, 20 * s, ImVec2((x + w - pad) * s - msz.x, (y + pad + 8) * s), IM_COL32(244, 245, 247, 255), ms);
  if (simple) return;
  dl->AddText(body, 14 * s, P(x + pad, y + pad + 37), IM_COL32(255, 255, 255, 175), small);
  for (size_t i = 0; i < info.size(); ++i)
    dl->AddText(body, 14 * s, P(x + pad, y + pad + 37 + 20 + float(i) * line_h), IM_COL32(255, 255, 255, 175),
                info[i].c_str());

  // the graph: one bar a frame (as the TV showed them, when known), newest
  // on the right, 0 to 66 ms (15 FPS) tall
  const float gx = x + pad, gy = y + pad + 36 + 20 + float(info.size()) * line_h + 10, gw = w - pad * 2;
  dl->AddRectFilled(P(gx, gy), P(gx + gw, gy + graph_h), IM_COL32(255, 255, 255, 18), 4 * s);
  constexpr float kTopMs = 66.7f;
  const uint64_t frames = from_tv_ ? rex::frame_stats::g_tv_shown.load(std::memory_order_acquire)
                                   : rex::frame_stats::g_presents.load(std::memory_order_acquire);
  constexpr int kBars = 120;
  const float bar = gw / float(kBars);
  for (int i = 0; i < kBars && uint64_t(i) + 1 < frames; ++i) {
    const uint32_t us =
        from_tv_ ? rex::frame_stats::g_tv_hold_us[(frames - 1 - uint64_t(i)) % rex::frame_stats::kTvHistory].load(
                       std::memory_order_relaxed)
                 : rex::frame_stats::g_history_us[(frames - 1 - uint64_t(i)) % rex::frame_stats::kHistory].load(
                       std::memory_order_relaxed);
    if (!us) continue;
    const float f_ms = float(us) / 1000.0f;
    const float bh = graph_h * std::min(f_ms, kTopMs) / kTopMs;
    const float bx = gx + gw - bar * float(i + 1);
    dl->AddRectFilled(P(bx, gy + graph_h - bh), P(bx + bar * 0.8f, gy + graph_h), RateColor(f_ms));
  }
  for (float line_ms : {16.7f, 33.3f}) {  // the 60 and 30 FPS marks, over the bars
    const float ly = gy + graph_h - graph_h * line_ms / kTopMs;
    dl->AddLine(P(gx, ly), P(gx + gw, ly), IM_COL32(0, 0, 0, 140), 2 * s);
    dl->AddLine(P(gx, ly), P(gx + gw, ly), IM_COL32(255, 255, 255, 90), 1 * s);
    const char* label = line_ms < 20 ? "60" : "30";
    dl->AddText(bold, 11 * s, P(gx + 3 + 1, ly - 13 + 1), IM_COL32(0, 0, 0, 200), label);
    dl->AddText(bold, 11 * s, P(gx + 3, ly - 13), IM_COL32(255, 255, 255, 200), label);
  }
}

}  // namespace gta4ps5

// GTA IV PS5 - what the PS5 build draws over the game: the frame rate counter
// when it's on in the hub's Display settings (on the left edge: frames a
// second, the frame time, the slowest frames and a graph of the last frames),
// when it's on in the hub's Debug settings a frame time capture to /app0/logs,
// and the notice shown if a Return to Hub could not finish.
//
// The Return to Hub confirmation itself is the retail pause-menu message box
// (see gta4_frontend_hooks.cpp); this overlay only carries its accepted
// request to the host and reports a failed teardown.
#pragma once

#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace gta4ps5 {

class GameOverlay final : public rex::ui::ImGuiDialog {
 public:
  // `return_to_hub` runs once per accepted confirmation; it starts the
  // host-side session restart. `set_title_input_captured` freezes guest
  // keyboard/pointer input once the session is draining.
  GameOverlay(rex::ui::ImGuiDrawer* drawer, std::function<void()> return_to_hub,
              std::function<void(bool)> set_title_input_captured);
  ~GameOverlay() override;

  // Drops a latched request (the host refused or failed the return) so the
  // player can keep playing or try again.
  void ResetReturnPrompt();

  // A return that already committed to terminating the title could not finish
  // tearing the session down. The title can no longer be resumed safely, so
  // the overlay says so and keeps guest input frozen instead of pretending the
  // game is playable.
  void ShowReturnFailure();

 protected:
  void OnDraw(ImGuiIO& io) override;
  // [ps5 port] Drawn with each game frame, not every vblank: repainting every
  // vblank kept the TV's queue full of repeats, and each new frame waited
  // behind two of them (two vblanks of input lag).
  bool WantsContinuousRepaint() const override;

 private:
  void Sample();
  void Capture();
  void UpdateReturnRequest();
  void DrawReturnFailure(ImGuiIO& io);
  void SetTitleInputCaptured(bool captured);

  // the counter: averaged over half a second, from what the TV showed
  // (VideoOut's flips) when the pacing thread has it, else the game's frames
  uint64_t window_frames_ = 0, window_tv_ = 0;
  int64_t window_start_ns_ = 0;
  float fps_ = 0, ms_ = 0;
  float low_fps_ = 0, worst_ms_ = 0;  // (the 1% slowest frames, and the slowest)
  float game_fps_ = 0;                // the game's own frames a second
  uint32_t stutters_ = 0;             // frames the TV held too long, of the last 300
  bool from_tv_ = false;
  std::string resolution_;            // (read once a second: a by-name lookup locks)
  int64_t resolution_at_ns_ = 0;
  // the capture
  std::FILE* csv_ = nullptr;
  uint64_t captured_ = 0;
  int64_t capture_start_ns_ = 0;
  // F02 Return to Hub (the accepted request and a failed teardown)
  std::function<void()> return_to_hub_;
  std::function<void(bool)> set_title_input_;
  bool title_input_captured_ = false;
  bool return_failed_ = false;
};

// The build's version.
const char* BuildVersion();

}  // namespace gta4ps5

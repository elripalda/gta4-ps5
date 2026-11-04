// [ps5 port] GTA IV's achievements as trophies, in the game (see
// hub/hub_trophies.h for the whole of it):
//
//  - WriteCatalog: as the game starts, its achievements' list and pictures
//    (from default.xex) go to user_data/achievements, for the hub's Trophies page;
//  - TrophyPopup: an unlock shows in the top-right corner, where the PS5 shows
//    its trophies - the achievement's picture, its grade's cup and its name,
//    with a chime - instead of the runtime's toast. The last of GTA IV's
//    brings the platinum's after it.
#pragma once

#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace gta4ps5 {

void WriteTrophyCatalog(rex::Runtime* runtime, const std::filesystem::path& user_data);

class TrophyPopup final : public rex::ui::AchievementNotificationDialog {
 public:
  TrophyPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime);
  ~TrophyPopup() override;

  // (from the thread that unlocked it)
  void Push(const rex::system::AchievementEvent& event) override;
  // [ps5 port] Only while a card is up. The default (always) had the screen
  // redrawn at every vblank for the whole session: the frame on the TV sent
  // again ~50 times a second, new frames waiting behind those repeats and
  // landing one vblank early or late (1.1's logs: "TV 110 fps" at 60 FPS).
  bool WantsContinuousRepaint() const override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct Card {
    rex::system::AchievementInfo achievement;
    bool platinum = false;
    std::optional<std::chrono::steady_clock::time_point> shown;
  };
  void Chime(bool platinum);
  rex::ui::ImmediateTexture* TitlePicture();

  rex::Runtime* runtime_ = nullptr;
  rex::ui::ImmediateDrawer* immediate_ = nullptr;
  rex::ui::AchievementIconCache icons_;
  std::unique_ptr<rex::ui::ImmediateTexture> title_picture_;
  bool title_picture_tried_ = false;
  mutable std::mutex mutex_;
  std::deque<Card> queue_;
};

}  // namespace gta4ps5

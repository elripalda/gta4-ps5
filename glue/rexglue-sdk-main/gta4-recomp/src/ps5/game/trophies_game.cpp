// [ps5 port] see trophies_game.h
#include "trophies_game.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>

#include "../hub/hub_lang.h"
#include "../hub/hub_sfx.h"
#include "../hub/hub_trophies.h"
#include "sound.h"
#if defined(__PROSPERO__)
#include "../hub/ps5_hub.h"
#endif

namespace gta4ps5 {

namespace fs = std::filesystem;

// ------------------------------------------------------------- the catalog

void WriteTrophyCatalog(rex::Runtime* runtime, const fs::path& user_data) {
  auto* ks = runtime ? runtime->kernel_state() : nullptr;
  if (!ks) return;
  std::vector<trophies::Trophy> list;
  int earned = 0;
  for (const auto& a : ks->achievements().ListAchievements()) {
    trophies::Trophy t;
    t.id = a.id, t.gamerscore = a.gamerscore, t.flags = a.flags, t.image_id = a.image_id;
    t.name = a.label, t.description = a.description, t.locked_description = a.unachieved_description;
    earned += ks->achievements().IsUnlocked(a.id) ? 1 : 0;
    list.push_back(std::move(t));
  }
  std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  if (list.empty()) {
    REXLOG_WARN("trophies: the game has no achievements loaded");
    return;
  }
  const bool ok = trophies::WriteCatalog(user_data, list);
  // their pictures (PNGs in default.xex), once
  int pictures = 0;
  const auto db = ks->title_xdbf();
  auto save = [&](const fs::path& file, const rex::system::util::XdbfBlock& block) {
    std::error_code ec;
    if (!block || fs::exists(file, ec)) return;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
    ++pictures;
  };
  if (db.is_valid()) {
    for (const auto& t : list)
      if (t.image_id)
        save(trophies::IconFile(user_data, t.image_id),
             db.GetEntry(rex::system::util::XdbfSection::kImage, uint64_t(t.image_id)));
    save(trophies::TitleIconFile(user_data), db.icon());
  }
  REXLOG_INFO("trophies: {} achievements, {} earned; the list {} ({} new pictures) in {}", list.size(), earned,
              ok ? "written" : "NOT written", pictures, trophies::Folder(user_data).string());
}

// ---------------------------------------------------------------- the popup

namespace {

struct Fonts {
  ImFont* body = nullptr;
  ImFont* bold = nullptr;
};

Fonts GetFonts() {
  Fonts f;
#if defined(__PROSPERO__)
  f.body = hub::Fonts().body, f.bold = hub::Fonts().body_bold;
#endif
  if (!f.body) f.body = ImGui::GetFont();
  if (!f.bold) f.bold = f.body;
  return f;
}

}  // namespace

TrophyPopup::TrophyPopup(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), runtime_(runtime), immediate_(immediate), icons_(immediate, runtime) {}

TrophyPopup::~TrophyPopup() = default;

void TrophyPopup::Push(const rex::system::AchievementEvent& event) {
  // the last one that can be earned here: the platinum after it
  bool platinum = false;
  if (trophies::Here(event.achievement.id) && runtime_ && runtime_->kernel_state()) {
    auto& m = runtime_->kernel_state()->achievements();
    int earnable = 0, earned = 0;
    for (const auto& a : m.ListAchievements()) {
      if (!trophies::Here(a.id)) continue;
      ++earnable;
      earned += m.IsUnlocked(a.id) ? 1 : 0;
    }
    platinum = earnable > 0 && earned == earnable;
  }
  REXLOG_INFO("trophies: {} ({}, {}G){}", event.achievement.label,
              trophies::GradeName(trophies::GradeFor(event.achievement.gamerscore)), event.achievement.gamerscore,
              platinum ? ", and with it the platinum" : "");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({event.achievement, false, std::nullopt});
    if (platinum) queue_.push_back({event.achievement, true, std::nullopt});
  }
  RequestRepaint();
}

bool TrophyPopup::WantsContinuousRepaint() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !queue_.empty();
}

void TrophyPopup::Chime(bool platinum) {
  static auto* voice = [] {
    sound::StartOutput();
    return sound::NewVoice();
  }();
  static const auto chime = std::make_shared<const std::vector<float>>(hub::TrophyChime(false));
  static const auto chime_platinum = std::make_shared<const std::vector<float>>(hub::TrophyChime(true));
  voice->SetVolume(0.9f);
  voice->Play(sound::Memory(platinum ? chime_platinum : chime), false);
}

rex::ui::ImmediateTexture* TrophyPopup::TitlePicture() {
  if (title_picture_tried_) return title_picture_.get();
  title_picture_tried_ = true;
  if (!immediate_ || !runtime_ || !runtime_->kernel_state()) return nullptr;
  const auto db = runtime_->kernel_state()->title_xdbf();
  if (!db.is_valid()) return nullptr;
  const auto block = db.icon();
  if (!block) return nullptr;
  int w = 0, h = 0;
  const std::vector<uint8_t> px = rex::ui::DecodeImageRGBA(block.buffer, block.size, w, h);
  if (px.empty()) return nullptr;
  title_picture_ = immediate_->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                             px.data());
  return title_picture_.get();
}

void TrophyPopup::OnDraw(ImGuiIO& io) {
  const auto now = std::chrono::steady_clock::now();
  Card card;
  float age = 0;
  bool start = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      Card& front = queue_.front();
      if (!front.shown) front.shown = now, start = true;
      age = std::chrono::duration<float>(now - *front.shown).count();
      if (age < (front.platinum ? 7.0f : 5.5f)) {
        card = front;
        break;
      }
      queue_.pop_front();
    }
    if (queue_.empty()) return;
  }
  if (start) Chime(card.platinum);
  trophies::Card view;
  view.platinum = card.platinum;
  view.grade = card.platinum ? trophies::Grade::kPlatinum : trophies::GradeFor(card.achievement.gamerscore);
  view.name = card.platinum ? hub::lang::T("Grand Theft Auto IV Platinum") : card.achievement.label;
  view.picture = reinterpret_cast<ImTextureID>(card.platinum ? TitlePicture() : icons_.GetIcon(card.achievement));
  const Fonts f = GetFonts();
  trophies::DrawCard(ImGui::GetForegroundDrawList(), io.DisplaySize, view, age, f.body, f.bold);
}

}  // namespace gta4ps5

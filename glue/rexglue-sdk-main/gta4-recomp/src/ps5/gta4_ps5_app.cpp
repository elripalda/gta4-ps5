// GTA IV PS5 - the app's PS5 parts (see gta4_ps5_app.h).
#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include "gta4_ps5_app.h"

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>

#include "../gta4_app.h"
#include "trophies_native/native_trophies.h"
#include "game/ps5_display.h"
#include "game/ps5_overlay.h"
#include "game/save_import.h"
#include "game/trophies_game.h"
#include "hub/hub_lang.h"
#include "hub/ps5_hub.h"
#include "lifecycle/f02_menu_request.h"

// The hub's Language (Game page): auto (the PS5's), english, french, german,
// spanish or italian. The game is told it as the Xbox 360's language and
// reads its text from the matching .gxt (the USA disc has all five).
REXCVAR_DEFINE_STRING(ps5_language, "auto", "PS5", "Game and hub language: auto, english, french, german, spanish, italian");
// The hub's City Population (Display page): the game's four population
// scales together (gta4_population_hooks.cpp), set at the start.
REXCVAR_DEFINE_DOUBLE(ps5_population, 1.0, "PS5", "Cars and people in the city: 1.0 is the Xbox 360's").range(0.25, 2.0);

namespace gta4ps5 {

namespace fs = std::filesystem;

namespace {
const fs::path kApp("/app0");
fs::path g_saves_root;
std::unique_ptr<GameOverlay> g_overlay;
std::function<void()> g_return_to_hub;
}  // namespace

void ShowHub(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const rex::PathConfig& paths,
             const fs::path& liberty_root, const fs::path& config, std::function<void()> launch) {
  g_saves_root = paths.saved_game_root.empty() ? liberty_root / "saves" : paths.saved_game_root;
  std::error_code ec;
  fs::create_directories(config.parent_path(), ec);
  fs::create_directories(SaveImportFolder(kApp), ec);
  hub::Paths hub_paths;
  hub_paths.app = kApp;
  hub_paths.config = config;
  hub_paths.user_data = g_saves_root;
  hub_paths.install = liberty_root;
  hub::SetSaveImport({[] { return PendingSaveImports(kApp); },
                      [] {
                        const SaveImportResult r = ImportXbox360Saves(kApp, g_saves_root);
                        namespace lang = hub::lang;
                        std::string message;
                        if (r.imported)
                          message = lang::F(r.imported == 1 ? "{} save imported." : "{} saves imported.",
                                            {std::to_string(r.imported)}) +
                                    " " + lang::T("Load them from the game's menu.");
                        else
                          message = lang::T("No saves were imported.");
                        for (size_t i = 0; i < r.errors.size() && i < 4; ++i) message += "\n" + r.errors[i];
                        if (r.errors.size() > 4) message += std::string("\n") + lang::T("...the log lists the rest.");
                        return message;
                      }});
  hub::SetDisplay({[] { return display::HighRefresh(); }, [](bool high) { return display::SetHighRefresh(high); },
                   [] { return display::Vrr(); }, [] { return display::Ready(); }});
  hub::Show(drawer, immediate, hub_paths, [config, launch = std::move(launch)] {
    // the hub's settings, for this start
    rex::cvar::LoadConfig(config);
    // the language: the game reads it as the console's (XCONFIG user language)
    const hub::lang::Lang language = hub::lang::FromSetting(REXCVAR_GET(ps5_language));
    REXCVAR_SET(user_language, hub::lang::XboxLanguage(language));
    REXLOG_INFO("PS5 hub: language {} (setting {}, Xbox language {})", hub::lang::Name(language),
                REXCVAR_GET(ps5_language), REXCVAR_GET(user_language));
    // the city's population
    const std::string population = fmt::format("{}", REXCVAR_GET(ps5_population));
    for (const char* scale : {"gta4_traffic_density_scale", "gta4_parked_car_density_scale", "gta4_ped_density_scale",
                              "gta4_scenario_ped_density_scale"})
      rex::cvar::SetFlagByName(scale, population);
    REXLOG_INFO("PS5 hub: city population {}", population);
    REXLOG_INFO("PS5 hub: START GAME (GTA IV PS5 {})", BuildVersion());
    // (the frame limit's pacing on the TV, from now on)
    display::StartGamePacing();
    launch();
  });
}

void OnGameSetUp(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime, const fs::path& user_data,
                 std::function<void(bool)> set_title_input_captured) {
  WriteTrophyCatalog(runtime, user_data);
  // (the game's PS5 trophy set and pop-ups, on a thread of its own)
  StartNativeTrophies(runtime, user_data);
  // A new session starts with nothing latched: the previous confirmation (if
  // any) is gone with the frontend it belonged to.
  gta4::ps5::lifecycle::f02::RequestConfirmationReset();
  g_overlay = std::make_unique<GameOverlay>(drawer, g_return_to_hub,
                                            std::move(set_title_input_captured));
}

void SetReturnToHubHandler(std::function<void()> handler) {
  g_return_to_hub = std::move(handler);
}

void OnGameTearDown() {
  // The dialog goes before the drawer; the pictures go before the immediate
  // drawer that made them.
  g_overlay.reset();
  hub::ResetTextures();
}

void ResetReturnPrompt() {
  if (g_overlay) g_overlay->ResetReturnPrompt();
}

void ShowReturnFailure() {
  if (g_overlay) {
    g_overlay->ShowReturnFailure();
  } else {
    // The overlay was already released with the presentation; there is nothing
    // left to tell the player through.
    REXLOG_ERROR("F02: the return failed after the presentation was released");
  }
}

}  // namespace gta4ps5

void GTA4App::OnConfigureFonts(ImFontAtlas* atlas) { gta4ps5::hub::ConfigureFonts(atlas); }

// [ps5 port] 1.1: no trophy popup in the game: the PS5 shows its own
// (trophies_native/native_trophies.h). Trophies are still earned and recorded as before.
std::unique_ptr<rex::ui::AchievementNotificationDialog> GTA4App::CreateAchievementNotificationDialog() {
  return nullptr;
}

#endif  // REX_PLATFORM_PS5

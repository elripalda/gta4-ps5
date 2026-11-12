// GTA IV PS5 - the app's PS5 parts: the hub before the game, the trophy
// popup, the frame rate counter and watermark, Xbox 360 save import.
#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_PS5
#include <filesystem>
#include <functional>

#include <rex/rex_app.h>

namespace gta4ps5 {

// Shows the hub (on the UI thread); `launch` runs on START GAME.
void ShowHub(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const rex::PathConfig& paths,
             const std::filesystem::path& liberty_root, const std::filesystem::path& config,
             std::function<void()> launch);
// The runtime is set up: the trophy list for the hub, the in-game overlay.
// `set_title_input_captured` freezes guest input while the Return to Hub
// confirmation is up.
void OnGameSetUp(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime, const std::filesystem::path& user_data,
                 std::function<void(bool)> set_title_input_captured);
// The pause menu's Return to Hub action: runs `handler` once, on the UI thread.
void SetReturnToHubHandler(std::function<void()> handler);
// The current session is ending and its presentation is being released: drop
// the in-game overlay and the hub's pictures before the drawer that made them
// is destroyed.
void OnGameTearDown();
// The host refused or could not complete a return: release the latched prompt
// so the player can keep playing.
void ResetReturnPrompt();
// The host committed to ending the title but could not finish the teardown:
// the session is no longer resumable, so the overlay says so and freezes input.
void ShowReturnFailure();

}  // namespace gta4ps5
#endif

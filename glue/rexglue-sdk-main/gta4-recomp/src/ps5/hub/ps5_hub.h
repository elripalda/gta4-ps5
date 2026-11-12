// GTA IV PS5 - the PS5 hub on the console (ps5_hub.cpp).
#pragma once

#if defined(__PROSPERO__)
#include <functional>

#include "hub_ui.h"

struct ImFontAtlas;
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace gta4ps5::hub {
// The hub's fonts (from OnConfigureFonts).
void ConfigureFonts(ImFontAtlas* atlas);
// The hub's fonts, for the port's things in the game (the trophy popup).
const Assets& Fonts();
// Shows the hub; `launch` runs (on the UI thread) on START GAME.
void Show(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const Paths& paths,
          std::function<void()> launch);
// Drops the pictures loaded by Show, before the ImmediateDrawer that made them
// is destroyed (an in-process return to the hub rebuilds the drawer). The next
// Show loads them again.
void ResetTextures();
// Xbox 360 save import on the Saves page (before Show).
void SetSaveImport(Hub::SaveImport import);
// The Refresh Rate's switch on the Display page (before Show).
void SetDisplay(Hub::Display display);
// The port's version ("1.0t1": the hub shows "1.0 Test 1"), from ps5/VERSION.txt.
const char* Version();
}  // namespace gta4ps5::hub
#endif

// [ps5 port] GTA IV in the PS5's own Trophies list, set up by the game itself:
// at each start, on a thread of its own, it does what pressing Cross for GTA IV
// in RecompTrophy does (its code: install and trophyset here, its payloads:
// ps5/payload/trophies), when something changed since the last time:
//
//  - makes the game's trophy set from its achievements (the running game's
//    XDBF, the same data RecompTrophy reads from default.xex);
//  - has RecompTrophy's relay install it and bring in the achievements
//    already earned, with their dates;
//  - sets up RecompTrophy's watcher for the game's achievements file (by its
//    real path, /data/homebrew/...), starts it, and puts it in the Payload
//    Manager's autoload list: from then on the PS5's own trophy pop-ups.
//
// Needs a payload loader on port 9021 (etaHEN, elfldr...); without one it
// does nothing. The hub's PS5 Trophies setting (ps5_trophies) turns it off.
// Logged as "ps5 trophies: ...".
#pragma once

#include <filesystem>

namespace rex {
class Runtime;
}

namespace gta4ps5 {

// From the game's setup (OnGameSetUp): returns at once.
void StartNativeTrophies(rex::Runtime* runtime, const std::filesystem::path& user_data);

}  // namespace gta4ps5

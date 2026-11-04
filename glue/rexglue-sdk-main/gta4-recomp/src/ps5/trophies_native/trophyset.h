// From RecompTrophy by lamarking (https://github.com/cptlamarca/RecompTrophy),
// GPL-3.0, used with permission (commit f78630c). Changed for the port: in the
// recomptrophy namespace.
//
// A game's PS5 trophy set (TROPHY.UCP) from its achievements: trophies 1..N
// are its achievements in the order of their ids (TrophyId; the console's
// trophy list wants them numbered without gaps, and some games' achievements
// have gaps: Ghost Recon Future Soldier's are 13..65), plus a platinum (0)
// for all of them; the grade by gamerscore
// (<=10 bronze, <=25 silver, else gold); pictures scaled to 512x512 (bicubic).
//
// The container (no key): a 0x60 byte header (magic B228C60A, version 1, size,
// count, entry size 0x40, the SHA-1 of the file with it zeroed at 0x1C), a
// table of 64 byte entries (32 byte name, offset, size) and the files, each
// padded to 16 bytes: icon0_en-US.png, trop%04d.png, tropconf.json,
// tropmeta_<lang>.json. The same as the ports make (aotdc-ps5's
// src/ps5_trophy_set.cpp), so a port and this app make the same set.
//
// Also trophies.txt ("ID GRADE HIDDEN PLATINUM_ID", grade 1 platinum .. 4
// bronze): the list the relay makes the player's progress file from.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game.h"

namespace recomptrophy::trophyset {

// The trophy set id of an Xbox 360 title: a made-up NPWR9xxxx_00 from its
// title id (the ports use the same: Army of Two: The Devil's Cartel's,
// published before the rule, keeps NPWR99990_00).
std::string SetId(uint32_t title_id);

// The trophy of an achievement (0 when it isn't one of the game's).
uint32_t TrophyId(const game::Info& info, uint32_t achievement_id);

// icon_png: the set's picture (a port's icon), else the game's from its XDBF.
bool Build(const game::Info& info, const std::string& set_id, const std::string& title_name,
           const std::vector<uint8_t>& icon_png, const std::string& ucp_path, const std::string& list_path,
           float* progress);

}  // namespace recomptrophy::trophyset

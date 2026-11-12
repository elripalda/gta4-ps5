#pragma once

#include <cstdint>

namespace gta4::episode_startup {

// The hub writes gta4_start_episode before every session launch. The start
// consumes that choice once per guest session: without a reset the second
// session never sees its own choice and falls back to retail's newest-save
// scan, which can load another episode.
struct SessionState {
  int32_t answer_yes_for = -1;  // (the episode whose question is answered)
  bool start_episode_used = false;  // (the start only: later checks are the game's)
  int32_t boot_episode = -1;    // (the Home choice, while the start's steps run)
  bool in_start_steps = false;  // (inside sub_822440F8)

  bool ConsumeHomeChoice(int32_t start_episode) {
    if (start_episode < 0 || start_episode_used) return false;
    start_episode_used = true;
    return true;
  }

  void Reset() { *this = SessionState{}; }
};

// One instance for the process; the frontend hooks and the session lifecycle
// share it.
inline SessionState& Session() {
  static SessionState state;
  return state;
}

}  // namespace gta4::episode_startup

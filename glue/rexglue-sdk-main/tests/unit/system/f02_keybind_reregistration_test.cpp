// F02 host test: the keybind registry survives an in-process session restart.
//
// An in-process return to the hub rebuilds the overlay dialogs, which
// re-registers the same named binds (debug overlay, console, settings,
// achievements, team/all chat). The registry is global and backed by CVARs, so
// a naive second registration would either be ignored (new callback lost) or
// clobber a user-rebound key. This test compiles the real src/ui/keybinds.cpp
// against small cvar/logging/input-trace stubs and checks the actual registry
// behavior, including a simulated user rebind.
//
// Build (from glue/rexglue-sdk-main):
//   g++ -std=c++20 -O2 -Wall -Wextra -Werror -DGTA4_F02_STANDALONE
//       -I tests/unit/system/f02_keybind_stubs -I include
//       tests/unit/system/f02_keybind_reregistration_test.cpp
//       src/ui/keybinds.cpp -o /tmp/f02_keybind
//   /tmp/f02_keybind

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

namespace {

struct RegisteredFlag {
  rex::cvar::FlagDescriptor descriptor;
};

std::vector<RegisteredFlag> g_flags;
int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

rex::ui::KeyEvent Key(rex::ui::VirtualKey key, bool repeat) {
  return rex::ui::KeyEvent(nullptr, key, repeat ? 1 : 0, repeat, false, false, false, false);
}

}  // namespace

namespace rex::cvar {

// The real registry is replaced so the test can see how many flags were
// created and drive the setter/getter the keybinds module installed.
void RegisterFlag(FlagDescriptor descriptor) {
  g_flags.push_back({std::move(descriptor)});
}

}  // namespace rex::cvar

namespace {

void Run() {
  using rex::ui::VirtualKey;

  int first_calls = 0;
  int second_calls = 0;

  rex::ui::RegisterBind("bind_f02_test", "F3", "F02 test bind", [&] { ++first_calls; });
  Check(g_flags.size() == 1, "first registration creates exactly one flag");
  Check(g_flags[0].descriptor.name == "bind_f02_test", "flag keeps the bind name");
  Check(g_flags[0].descriptor.default_value == "F3", "flag keeps the default key");
  Check(g_flags[0].descriptor.getter() == "F3", "the bind starts on its default key");

  // The rebuilt dialogs register again with the same name.
  rex::ui::RegisterBind("bind_f02_test", "F4", "F02 test bind", [&] { ++second_calls; });
  Check(g_flags.size() == 1, "re-registration does not create a second flag");
  Check(g_flags[0].descriptor.default_value == "F3",
        "re-registration does not replace the stored default");
  Check(g_flags[0].descriptor.getter() == "F3",
        "re-registration does not reset the active key to the new default");

  {
    auto event = Key(VirtualKey::kF3, false);
    const bool handled = rex::ui::ProcessKeyEvent(event);
    Check(handled, "the revived bind consumes its key");
    Check(first_calls == 0, "the old (destroyed) dialog's callback is not kept");
    Check(second_calls == 1, "the new session's callback is dispatched");
  }

  // A user rebind through the cvar setter (as the settings overlay does).
  Check(g_flags[0].descriptor.setter("F5"), "the cvar setter accepts a rebind");
  Check(g_flags[0].descriptor.getter() == "F5", "the rebind is visible through the getter");
  rex::ui::RegisterBind("bind_f02_test", "F3", "F02 test bind", [&] { ++second_calls; });
  Check(g_flags.size() == 1, "a rebind plus re-registration still has one flag");
  Check(g_flags[0].descriptor.getter() == "F5",
        "re-registration preserves the user's rebound key");
  {
    auto old_key = Key(VirtualKey::kF3, false);
    Check(!rex::ui::ProcessKeyEvent(old_key), "the abandoned default key no longer fires");
    auto new_key = Key(VirtualKey::kF5, false);
    Check(rex::ui::ProcessKeyEvent(new_key), "the rebound key fires after re-registration");
  }
  Check(second_calls == 2, "the rebound key reaches the current callback");

  // A held key never double-fires a one-shot bind.
  {
    auto held = Key(VirtualKey::kF5, true);
    Check(!rex::ui::ProcessKeyEvent(held), "a repeat key-down is not dispatched");
  }
  Check(second_calls == 2, "the repeat did not call the callback again");

  // Unregister (the old session's shutdown) then register again (the new
  // session's dialogs): the entry is revived without losing the key.
  rex::ui::UnregisterBind("bind_f02_test");
  {
    auto event = Key(VirtualKey::kF5, false);
    Check(!rex::ui::ProcessKeyEvent(event), "an unregistered bind does not fire");
  }
  int third_calls = 0;
  rex::ui::RegisterBind("bind_f02_test", "F6", "F02 test bind", [&] { ++third_calls; });
  Check(g_flags.size() == 1, "reviving after unregister reuses the entry");
  Check(g_flags[0].descriptor.getter() == "F5", "reviving after unregister keeps the key");
  {
    auto event = Key(VirtualKey::kF5, false);
    Check(rex::ui::ProcessKeyEvent(event), "the revived bind fires again");
  }
  Check(third_calls == 1, "the revived callback is the one that runs");

  // A genuinely new bind still gets its own flag.
  int other_calls = 0;
  rex::ui::RegisterBind("bind_f02_other", "F7", "F02 other bind", [&] { ++other_calls; });
  Check(g_flags.size() == 2, "a different name creates a second flag");
  {
    auto event = Key(VirtualKey::kF7, false);
    Check(rex::ui::ProcessKeyEvent(event), "the second bind fires on its own key");
  }
  Check(other_calls == 1, "the second bind's callback ran once");
  Check(third_calls == 1, "the first bind's callback did not run for another key");
}

}  // namespace

#if defined(GTA4_F02_STANDALONE)
int main() {
  Run();
  std::printf("%d checks, %d failed check(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
#include <catch2/catch_test_macros.hpp>

TEST_CASE("F02 keybind re-registration keeps one entry and the user's key", "[f02][keybinds]") {
  g_flags.clear();
  g_checks = 0;
  g_failures = 0;
  Run();
  // Catch2 reports a single aggregate check so the standalone and Catch2 runs
  // assert the same outcome.
  REQUIRE(g_failures == 0);
  REQUIRE(g_checks > 0);
  g_flags.clear();
}
#endif

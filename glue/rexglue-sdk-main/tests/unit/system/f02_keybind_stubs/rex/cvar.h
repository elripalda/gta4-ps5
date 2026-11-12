// Minimal cvar surface for the F02 keybind registry test. Only what
// src/ui/keybinds.cpp uses; the field order must match its designated
// initializers. The real registry is not linked so the test can inspect how
// many flags a re-registration produced.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace rex::cvar {

enum class FlagType { Bool, Integer, Double, String };
enum class Lifecycle { kStatic, kHotReload };

struct FlagDescriptor {
  std::string name;
  FlagType type;
  std::string category;
  std::string description;
  std::function<bool(std::string_view)> setter;
  std::function<std::string()> getter;
  Lifecycle lifecycle;
  std::string default_value;
};

void RegisterFlag(FlagDescriptor descriptor);

}  // namespace rex::cvar

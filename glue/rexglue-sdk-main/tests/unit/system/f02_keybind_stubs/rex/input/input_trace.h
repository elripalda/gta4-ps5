// Input tracing is off in the keybind registry test.
#pragma once

#include <cstdint>

namespace rex::input {

inline bool IsInputTraceEnabled() {
  return false;
}

inline const char* InputTraceVirtualKeyName(uint32_t) {
  return "";
}

}  // namespace rex::input

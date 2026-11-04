#pragma once
// [ps5 port] A by-name setting read at most every 250 ms per thread and call
// site. rex::cvar::GetFlagByName and Query lock the registry every other
// thread shares and copy strings: called while frames are built, they cost
// the recorder up to a quarter of its time in 1.0.2 players' profiles. The
// settings read here change only from the hub (applied at the next start) or
// the frontend, where a quarter second late is fine.
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <rex/cvar.h>

namespace rex::graphics::gta4_native {

template <typename T>
T CachedQuery(const char* name) {
  thread_local std::unordered_map<const char*, std::pair<std::chrono::steady_clock::time_point, T>> cache;
  const auto now = std::chrono::steady_clock::now();
  auto it = cache.find(name);
  if (it == cache.end() || now - it->second.first > std::chrono::milliseconds(250)) {
    T value = rex::cvar::Query<T>(name);
    it = cache.insert_or_assign(name, std::make_pair(now, std::move(value))).first;
  }
  return it->second.second;
}

inline std::string CachedFlagByName(const char* name) { return CachedQuery<std::string>(name); }

}  // namespace rex::graphics::gta4_native

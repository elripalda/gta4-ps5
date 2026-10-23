#pragma once

// Recording API doubles for mod_loader_test.cpp; these do not emulate VFS lookup.
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rex {
namespace cvar {
enum class Lifecycle { kRequiresRestart };
struct Flag {
  Flag& lifecycle(Lifecycle) { return *this; }
};
}  // namespace cvar

namespace test {
inline bool initialize_success = true;
template <typename... Args> void Log(Args&&...) {}
}  // namespace test

namespace filesystem {
struct Device {
  virtual ~Device() = default;
  virtual bool Initialize() = 0;
};
struct HostPathDevice : Device {
  HostPathDevice(std::string_view mount, const std::filesystem::path& root, bool read_only,
                 bool share_delete = false, bool trace = false, bool eager = true)
      : mount(mount), root(root), read_only(read_only), share_delete(share_delete),
        trace(trace), eager(eager) {}
  bool Initialize() override { return test::initialize_success; }
  std::string mount;
  std::filesystem::path root;
  bool read_only, share_delete, trace, eager;
};

struct VirtualFileSystem {
  bool RegisterDevice(std::unique_ptr<Device> device) {
    ++register_attempts;
    if (!register_success) return false;
    devices.push_back(std::move(device));
    return true;
  }
  bool RegisterSymbolicLink(std::string_view source, std::string_view target) {
    return links.emplace(std::string(source), std::string(target)).second;
  }
  bool UnregisterSymbolicLink(std::string_view source) {
    return links.erase(std::string(source)) != 0;
  }
  bool register_success = true;
  unsigned register_attempts = 0;
  std::vector<std::unique_ptr<Device>> devices;
  std::map<std::string, std::string> links;
};
}  // namespace filesystem

class Runtime {
 public:
  filesystem::VirtualFileSystem* file_system() { return &vfs; }
  filesystem::VirtualFileSystem vfs;
};

namespace string {
inline bool utf8_starts_with_case(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
  for (size_t i = 0; i < prefix.size(); ++i)
    if (lower(text[i]) != lower(prefix[i])) return false;
  return true;
}
}  // namespace string
}  // namespace rex

#define REXCVAR_DEFINE_BOOL(name, value, ...) \
  bool name = value; \
  static ::rex::cvar::Flag name##_registration = ::rex::cvar::Flag{}
#define REXCVAR_GET(name) (name)
#define REXLOG_INFO(...) ::rex::test::Log(__VA_ARGS__)
#define REXLOG_WARN(...) ::rex::test::Log(__VA_ARGS__)

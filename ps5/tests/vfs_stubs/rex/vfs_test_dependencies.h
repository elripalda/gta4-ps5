#pragma once

// Dependency doubles only: tests compile the production VFS implementation
// and class declaration, with no PS5 SDK, logging or host-file dependencies.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define REXCVAR_DEFINE_BOOL(name, value, ...) inline bool name = value
#define REXCVAR_DECLARE(type, name) extern type name
#define REXFS_DEBUG(...) ((void)0)
#define REXFS_ERROR(...) ((void)0)
#define REXFS_TRACE(...) ((void)0)
#define REXFS_WARN(...) ((void)0)
#define assert_always() assert(false)
#define XFAILED(status) ((status) != 0)

namespace rex {
using X_STATUS = uint32_t;
inline constexpr X_STATUS X_STATUS_SUCCESS = 0;
inline constexpr X_STATUS X_STATUS_NO_SUCH_FILE = 1;
inline constexpr X_STATUS X_STATUS_FILE_IS_A_DIRECTORY = 2;
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_COLLISION = 3;
inline constexpr X_STATUS X_STATUS_ACCESS_DENIED = 4;
inline std::filesystem::path to_path(std::string_view text) { return text; }

namespace thread {
struct global_critical_region {
  struct Guard { ~Guard() {} };
  Guard Acquire() { return {}; }
};
}  // namespace thread

namespace string {
inline bool utf8_equal_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
  for (size_t i = 0; i < a.size(); ++i)
    if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
inline bool utf8_starts_with_case(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && utf8_equal_case(text.substr(0, prefix.size()), prefix);
}
inline std::string utf8_canonicalize_guest_path(std::string_view path) { return std::string(path); }
inline std::vector<std::string_view> utf8_split_path(std::string_view path) {
  std::vector<std::string_view> parts;
  while (!path.empty()) {
    const size_t end = path.find('\\');
    const auto part = path.substr(0, end);
    if (!part.empty()) parts.push_back(part);
    if (end == std::string_view::npos) break;
    path.remove_prefix(end + 1);
  }
  return parts;
}
inline std::string utf8_join_guest_paths(std::string_view a, std::string_view b) {
  return std::string(a) + "\\" + std::string(b);
}
inline std::string utf8_find_base_guest_path(std::string_view path) {
  const size_t last = path.find_last_of('\\');
  return last == std::string_view::npos ? "" : std::string(path.substr(0, last));
}
inline std::string utf8_find_name_from_guest_path(std::string_view path) {
  return std::string(path.substr(path.find_last_of('\\') + 1));
}
}  // namespace string

namespace filesystem {
enum FileAccess : uint32_t {
  kFileReadData = 1, kFileWriteData = 2, kFileAppendData = 4,
  kGenericRead = 8, kGenericWrite = 16, kGenericAll = 32
};
enum class FileDisposition { kSuperscede, kOpen, kCreate, kOpenIf, kOverwrite, kOverwriteIf };
enum class FileAction { kSuperseded, kOpened, kCreated, kOverwritten, kExists, kDoesNotExist };
inline constexpr uint32_t kFileAttributeDirectory = 0x10;
inline constexpr uint32_t kFileAttributeNormal = 0x80;
struct File {};
class Entry {
 public:
  explicit Entry(std::string absolute_path = {}) : absolute_path_(std::move(absolute_path)) {}
  virtual ~Entry() = default;
  const std::string& absolute_path() const { return absolute_path_; }
  Entry* parent() const { return nullptr; }
  Entry* ResolvePath(std::string_view) { return nullptr; }
  Entry* GetChild(std::string_view) { return nullptr; }
  Entry* CreateEntry(std::string_view, uint32_t) { return nullptr; }
  bool Delete(Entry*) { return false; }
  bool Delete() { return false; }
  bool Truncate() { return false; }
  bool is_read_only() const { return true; }
  uint32_t attributes() const { return kFileAttributeNormal; }
  std::string name() const { return {}; }
  X_STATUS Open(uint32_t, File** out) { *out = &file; return X_STATUS_SUCCESS; }
  File file;
 private:
  std::string absolute_path_;
};
class Device {
 public:
  explicit Device(std::string_view mount) : mount_(mount) {}
  virtual ~Device() = default;
  const std::string& mount_path() const { return mount_; }
  virtual Entry* ResolvePath(std::string_view path) = 0;
 private:
  std::string mount_;
};
class HostPathEntry : public Entry {
 public:
  std::filesystem::path host_path() const { return {}; }
};
}  // namespace filesystem
}  // namespace rex

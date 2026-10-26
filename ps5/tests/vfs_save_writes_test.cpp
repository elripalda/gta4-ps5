/* Native Linux regression using production VFS, HostPathDevice and POSIX file I/O.
   Only logging and configuration registration are replaced. From the repository root:
g++ -std=c++23 -DFMT_HEADER_ONLY -DFMT_USE_CONSTEVAL=0 \
  -ffunction-sections -fdata-sections -pthread \
  -Ips5/tests/filesystem_stubs -Iglue/rexglue-sdk-main/include \
  -Iglue/rexglue-sdk-main/thirdparty/fmt/include \
  -Iglue/rexglue-sdk-main/thirdparty/simde \
  -Iglue/rexglue-sdk-main/thirdparty/utfcpp/source \
  ps5/tests/vfs_save_writes_test.cpp \
  glue/rexglue-sdk-main/src/filesystem/{virtual_file_system,device,entry}.cpp \
  glue/rexglue-sdk-main/src/filesystem/devices/host_path_{device,entry,file}.cpp \
  glue/rexglue-sdk-main/src/core/{filesystem_posix,utf8,string_buffer,mutex,mapped_memory_posix,filesystem_wildcard}.cpp \
  -Wl,--gc-sections -o /tmp/gta4-vfs-save-writes-test
/tmp/gta4-vfs-save-writes-test
*/
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>

namespace host = std::filesystem;
namespace fs = rex::filesystem;
using rex::X_STATUS;

struct TestFolder {
  host::path path = host::temp_directory_path() /
      ("gta4_vfs_saves_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFolder() { assert(host::create_directory(path)); }
  ~TestFolder() {
    std::error_code ec;
    host::remove_all(path, ec);
  }
};

void Seed(const host::path& path, std::string_view text) {
  host::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
  assert(out.good());
}

std::string ReadHost(const host::path& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::unique_ptr<fs::File> Open(fs::VirtualFileSystem& vfs, fs::Entry* root,
                             std::string_view path, fs::FileDisposition disposition,
                             uint32_t access, fs::FileAction expected_action) {
  fs::File* raw = nullptr;
  fs::FileAction action{};
  const auto status = vfs.OpenFile(root, path, disposition, access, false, true, &raw, &action);
  if (status != X_STATUS_SUCCESS)
    std::cerr << "OpenFile(" << path << ") failed: 0x" << std::hex << status << '\n';
  assert(status == X_STATUS_SUCCESS && raw && action == expected_action);
  return std::unique_ptr<fs::File>(raw);
}

void Write(fs::File& file, std::string_view text) {
  size_t count = 0;
  const auto bytes = std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  assert(file.WriteSync(bytes, 0, &count) == X_STATUS_SUCCESS && count == text.size());
}

void CheckRead(fs::VirtualFileSystem& vfs, std::string_view path, std::string_view expected) {
  auto file = Open(vfs, nullptr, path, fs::FileDisposition::kOpen,
                   fs::FileAccess::kGenericRead, fs::FileAction::kOpened);
  std::string text(expected.size(), '\0');
  size_t count = 0;
  assert(file->ReadSync(std::span(reinterpret_cast<uint8_t*>(text.data()), text.size()), 0, &count) ==
         X_STATUS_SUCCESS);
  assert(count == expected.size() && text == expected);
}

int main() {
  TestFolder temp;
  const auto saves = temp.path / "saves";
  const auto mods = temp.path / "mods";
  Seed(saves / "profile/slot.sav", "original save with trailing bytes");
  Seed(saves / "profile/overwrite.sav", "existing relative save");
  Seed(saves / "profile/target.sav", "aliased save");
  Seed(saves / "profile/nested/neighbor.sav", "nested neighbor unchanged");
  Seed(saves / "profile/neighbor.sav", "neighbor unchanged");
  Seed(mods / "slot.sav", "mod unchanged");

  fs::VirtualFileSystem vfs;
  // ContentManager uses a trailing separator and share-delete for writable save mounts.
  auto device = std::make_unique<fs::HostPathDevice>("\\Device\\Content\\1\\", saves, false, true);
  assert(device->Initialize());
  assert(vfs.RegisterDevice(std::move(device)));
  assert(vfs.RegisterSymbolicLink("save:", "\\Device\\Content\\1\\"));
  assert(vfs.RegisterSymbolicLink("save:\\profile\\alias.sav",
                                 "\\Device\\Content\\1\\profile\\target.sav"));
  assert(vfs.RegisterSymbolicLink("\\Device\\Content\\1\\profile\\linked",
                                 "\\Device\\Content\\1\\profile\\nested"));
  auto mod_device = std::make_unique<fs::HostPathDevice>("\\Device\\Mods", mods, true, false, false, false);
  assert(mod_device->Initialize());
  assert(vfs.RegisterDevice(std::move(mod_device)));
  assert(vfs.RegisterSymbolicLink("game:\\slot.sav", "\\Device\\Mods\\slot.sav"));

  auto* profile = vfs.ResolvePath("save:\\profile");
  assert(profile && !profile->is_read_only());

  // Existing writable saves still open relative to their directory handle.
  auto file = Open(vfs, profile, "slot.sav", fs::FileDisposition::kOpenIf,
                   fs::FileAccess::kGenericWrite, fs::FileAction::kOpened);
  Write(*file, "saved");
  assert(file->SetLength(5) == X_STATUS_SUCCESS);
  file.reset();
  assert(ReadHost(saves / "profile/slot.sav") == "saved");
  CheckRead(vfs, "save:\\profile\\slot.sav", "saved");

  // Absolute creation and overwrite remain the baseline for directory-relative writes.
  file = Open(vfs, nullptr, "save:\\profile\\absolute.sav", fs::FileDisposition::kCreate,
              fs::FileAccess::kGenericWrite, fs::FileAction::kCreated);
  Write(*file, "absolute original");
  file.reset();
  file = Open(vfs, nullptr, "save:\\profile\\absolute.sav", fs::FileDisposition::kOverwriteIf,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOverwritten);
  Write(*file, "abs");
  file.reset();
  assert(ReadHost(saves / "profile/absolute.sav") == "abs");

  // Overwriting an existing relative file used to delete it, then fail recreation.
  file = Open(vfs, profile, "overwrite.sav", fs::FileDisposition::kOverwrite,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOverwritten);
  Write(*file, "kept");
  file.reset();
  assert(ReadHost(saves / "profile/overwrite.sav") == "kept");

  file = Open(vfs, profile, "created.sav", fs::FileDisposition::kCreate,
              fs::FileAccess::kGenericWrite, fs::FileAction::kCreated);
  Write(*file, "relative original");
  file.reset();
  assert(ReadHost(saves / "profile/created.sav") == "relative original");
  file = Open(vfs, profile, "CREATED.SAV", fs::FileDisposition::kOverwriteIf,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOverwritten);
  Write(*file, "new");
  file.reset();
  assert(ReadHost(saves / "profile/created.sav") == "new");
  assert(!host::exists(saves / "profile/CREATED.SAV"));
  CheckRead(vfs, "save:\\profile\\created.sav", "new");

  file = Open(vfs, profile, "nested\\created.sav", fs::FileDisposition::kOpenIf,
              fs::FileAccess::kGenericWrite, fs::FileAction::kCreated);
  Write(*file, "nested original");
  file.reset();
  file = Open(vfs, profile, "nested\\created.sav", fs::FileDisposition::kOverwriteIf,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOverwritten);
  Write(*file, "nested");
  file.reset();
  assert(ReadHost(saves / "profile/nested/created.sav") == "nested");

  // Replacements retain the resolved target name, including through file aliases.
  file = Open(vfs, nullptr, "save:\\profile\\alias.sav", fs::FileDisposition::kOverwriteIf,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOverwritten);
  Write(*file, "target");
  file.reset();
  assert(ReadHost(saves / "profile/target.sav") == "target");
  assert(!host::exists(saves / "profile/alias.sav"));

  file = Open(vfs, nullptr, "\\Device\\Content\\1\\profile\\physical.sav",
              fs::FileDisposition::kCreate, fs::FileAccess::kGenericWrite,
              fs::FileAction::kCreated);
  Write(*file, "physical");
  file.reset();
  assert(ReadHost(saves / "profile/physical.sav") == "physical");

  // Parent lookup for new relative files also follows directory aliases.
  file = Open(vfs, profile, "linked\\new.sav", fs::FileDisposition::kCreate,
              fs::FileAccess::kGenericWrite, fs::FileAction::kCreated);
  Write(*file, "linked");
  file.reset();
  assert(ReadHost(saves / "profile/nested/new.sav") == "linked");
  assert(!host::exists(saves / "profile/linked"));

  // Game aliases cannot redirect saves or make the replacement writable.
  file = Open(vfs, nullptr, "game:\\slot.sav", fs::FileDisposition::kOpen,
              fs::FileAccess::kGenericWrite, fs::FileAction::kOpened);
  size_t count = 0;
  const uint8_t byte = 1;
  assert(file->WriteSync(std::span(&byte, 1), 0, &count) == X_STATUS_ACCESS_DENIED);
  assert(file->SetLength(0) == X_STATUS_ACCESS_DENIED);
  file.reset();
  assert(ReadHost(mods / "slot.sav") == "mod unchanged");
  assert(ReadHost(saves / "profile/neighbor.sav") == "neighbor unchanged");
  assert(ReadHost(saves / "profile/nested/neighbor.sav") == "nested neighbor unchanged");
  assert(!host::exists(temp.path / "created.sav"));
  assert(!host::exists(saves / "created.sav"));
}

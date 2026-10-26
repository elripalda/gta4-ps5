/* Run from the repository root:
g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-but-set-variable \
  -Ips5/tests/vfs_stubs -Iglue/rexglue-sdk-main/include \
  ps5/tests/vfs_mod_links_test.cpp \
  glue/rexglue-sdk-main/src/filesystem/virtual_file_system.cpp \
  -o /tmp/gta4-vfs-mod-links-test
/tmp/gta4-vfs-mod-links-test
*/
#include <rex/filesystem/vfs.h>

#include <cassert>
#include <memory>
#include <string>

namespace fs = rex::filesystem;

class RecordingDevice : public fs::Device {
 public:
  explicit RecordingDevice(std::string_view mount) : Device(mount) {}
  fs::Entry* ResolvePath(std::string_view path) override {
    last_path = path;
    return &entry;
  }
  fs::Entry entry;
  std::string last_path;
};

int main() {
  fs::VirtualFileSystem vfs;
  auto base_owner = std::make_unique<RecordingDevice>("\\Device\\Base");
  auto mods_owner = std::make_unique<RecordingDevice>("\\Device\\Mods");
  auto* base = base_owner.get();
  auto* mods = mods_owner.get();
  vfs.RegisterDevice(std::move(base_owner));
  vfs.RegisterDevice(std::move(mods_owner));
  vfs.RegisterSymbolicLink("game:", "\\Device\\Base");
  vfs.RegisterSymbolicLink("d:", "\\Device\\Base");
  vfs.RegisterSymbolicLink("game:\\common\\foo.dat", "\\Device\\Mods\\common\\foo.dat");
  vfs.RegisterSymbolicLink("\\Device\\Base\\common\\foo.dat", "\\Device\\Mods\\common\\foo.dat");

  const auto check = [&](std::string_view path, RecordingDevice* expected, std::string_view relative) {
    assert(vfs.ResolvePath(path) == &expected->entry);
    assert(rex::string::utf8_equal_case(expected->last_path, relative));
  };
  const auto check_link = [&](std::string_view path, std::string_view expected) {
    std::string target;
    assert(vfs.FindSymbolicLink(path, target));
    assert(target == expected);
  };

  // Exact replacement, mixed case, mount chaining and absolute device paths.
  check("game:\\common\\foo.dat", mods, "\\common\\foo.dat");
  check("GAME:\\COMMON\\FOO.DAT", mods, "\\common\\foo.dat");
  check("d:\\common\\foo.dat", mods, "\\common\\foo.dat");
  check("\\Device\\Base\\common\\foo.dat", mods, "\\common\\foo.dat");
  check_link("game:\\common\\foo.dat", "\\Device\\Mods\\common\\foo.dat");

  // Replacing foo.dat must leave similarly named base files available.
  for (const auto* path : {"game:\\common\\foo.dat2", "game:\\common\\foo.dat.bak",
                           "d:\\common\\foo.dat2", "\\Device\\Base\\common\\foo.dat2"})
    check(path, base, path == std::string_view("game:\\common\\foo.dat.bak")
                          ? "\\common\\foo.dat.bak" : "\\common\\foo.dat2");
  check_link("game:\\common\\foo.dat2", "\\Device\\Base");
  std::string target;
  assert(!vfs.FindSymbolicLink("\\Device\\Base\\common\\foo.dat2", target));

  // Directory links still match descendants, with or without trailing '\\'.
  vfs.RegisterSymbolicLink("game:\\data", "\\Device\\Mods\\data");
  vfs.RegisterSymbolicLink("game:\\ready\\", "\\Device\\Mods\\ready\\");
  check("game:\\data", mods, "\\data");
  check("game:\\data\\child.img", mods, "\\data\\child.img");
  check("game:\\database\\child.img", base, "\\database\\child.img");
  check_link("game:\\data\\child.img", "\\Device\\Mods\\data");
  check_link("game:\\database\\child.img", "\\Device\\Base");
  check("game:\\ready\\child.img", mods, "\\ready\\child.img");
  check_link("game:\\ready\\child.img", "\\Device\\Mods\\ready\\");
  check("game:", base, "");

  // OpenFile must resolve a file alias before looking up its parent directory.
  fs::File* opened = nullptr;
  fs::FileAction action;
  assert(vfs.OpenFile(nullptr, "game:\\common\\foo.dat", fs::FileDisposition::kOpen,
                      fs::FileAccess::kGenericRead, false, true, &opened, &action) ==
         rex::X_STATUS_SUCCESS);
  assert(opened == &mods->entry.file && action == fs::FileAction::kOpened);

  // A directory handle names the base device. Its relative children still
  // pass through the physical aliases, and prefix neighbors stay on base.
  fs::Entry base_root("\\Device\\Base");
  assert(vfs.OpenFile(&base_root, "common\\foo.dat", fs::FileDisposition::kOpen,
                      fs::FileAccess::kGenericRead, false, true, &opened, &action) ==
         rex::X_STATUS_SUCCESS);
  assert(opened == &mods->entry.file && action == fs::FileAction::kOpened);
  assert(vfs.OpenFile(&base_root, "common\\foo.dat2", fs::FileDisposition::kOpen,
                      fs::FileAccess::kGenericRead, false, true, &opened, &action) ==
         rex::X_STATUS_SUCCESS);
  assert(opened == &base->entry.file && action == fs::FileAction::kOpened);
}

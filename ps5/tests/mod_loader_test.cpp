// Host integration check: actual loader/scanner, recording ReXGlue API doubles.
/* From the repository root:
g++ -std=c++23 -Wall -Wextra -Werror -Ips5/tests/stubs \
  -Iglue/rexglue-sdk-main/gta4-recomp/src/ps5/game \
  ps5/tests/mod_loader_test.cpp \
  glue/rexglue-sdk-main/gta4-recomp/src/ps5/game/mod_{files,loader}.cpp \
  -o /tmp/gta4_mod_loader_test && /tmp/gta4_mod_loader_test
*/
#include "mod_loader.h"
#include <rex/runtime.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern bool ps5_enable_mods;
namespace fs = std::filesystem;

struct TestFolder {
  fs::path path = fs::temp_directory_path() /
      ("gta4_mod_loader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFolder() { assert(fs::create_directory(path)); }
  ~TestFolder() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void Write(const fs::path& path, std::string_view data) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << data;
  assert(file.good());
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  assert(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
  assert(!ps5_enable_mods);
  TestFolder temp;
  const fs::path root = temp.path / "mods/game";
  const fs::path installed = temp.path / "UserData/LibertyRecomp/game";
  Write(installed / "common/data/cdimages/script.img", "original script");
  Write(installed / "xbox360.rpf", "original textures");
  Write(root / "common/data/cdimages/script.img", "menu script");
  Write(root / "xbox360.rpf", "mod textures");
  Write(root / "UpDaTe/common/data/cdimages/script.img", "updated menu script");

  rex::Runtime disabled;
  ps5_enable_mods = false;
  gta4ps5::MountMods(disabled, temp.path);
  assert(disabled.vfs.register_attempts == 0 && disabled.vfs.links.empty());

  ps5_enable_mods = true;
  rex::Runtime missing;
  gta4ps5::MountMods(missing, temp.path / "missing_app");
  assert(missing.vfs.register_attempts == 0 && missing.vfs.links.empty());

  // Rejected files must not become guest aliases, even beside valid replacements.
  for (const char* name : {"menu.ASI", "menu.dll", "default.xex", "aes_key.bin",
                           "manifest.json", "README.txt", ".hidden.dat", "bad:drive/file.dat",
                           "bad\\name.dat", "collision.dat", "COLLISION.dat"})
    Write(root / name, "ignored");
  Write(temp.path / "outside.dat", "outside");
  fs::create_symlink(temp.path / "outside.dat", root / "linked.dat");

  rex::Runtime enabled;
  enabled.vfs.RegisterSymbolicLink("game:\\xbox360.rpf", "button_prompts");
  enabled.vfs.RegisterSymbolicLink("d:\\xbox360.rpf", "button_prompts");
  enabled.vfs.RegisterSymbolicLink("game:\\untouched.dat", "existing_override");
  gta4ps5::MountMods(enabled, temp.path);
  assert(enabled.vfs.register_attempts == 1 && enabled.vfs.devices.size() == 1);
  const auto* device = dynamic_cast<rex::filesystem::HostPathDevice*>(enabled.vfs.devices.front().get());
  assert(device && device->root == root && device->read_only);
  assert(!device->eager && !device->share_delete && !device->trace);
  assert(device->mount == "\\Device\\LibertyRecomp\\Mods");

  std::map<std::string, std::string> expected = {{"game:\\untouched.dat", "existing_override"}};
  for (const std::string relative : {"common\\data\\cdimages\\script.img", "xbox360.rpf",
                                     "UpDaTe\\common\\data\\cdimages\\script.img"}) {
    const std::string target = device->mount + "\\" + relative;
    for (const char* prefix : {"game:\\", "d:\\", "\\Device\\Harddisk0\\Partition1\\"})
      expected[std::string(prefix) + relative] = target;
    if (relative.starts_with("UpDaTe\\"))
      for (const char* prefix : {"update:\\", "\\Device\\Harddisk0\\PartitionUpdate\\"})
        expected[std::string(prefix) + relative.substr(7)] = target;
  }
  assert(enabled.vfs.links == expected);
  assert(Read(installed / "common/data/cdimages/script.img") == "original script");
  assert(Read(installed / "xbox360.rpf") == "original textures");
  assert(Read(root / "common/data/cdimages/script.img") == "menu script");

  // No aliases may be published when the backing device cannot be mounted.
  rex::Runtime failed_initialization;
  rex::test::initialize_success = false;
  gta4ps5::MountMods(failed_initialization, temp.path);
  assert(failed_initialization.vfs.register_attempts == 0 && failed_initialization.vfs.links.empty());
  rex::test::initialize_success = true;
  rex::Runtime failed_registration;
  failed_registration.vfs.register_success = false;
  gta4ps5::MountMods(failed_registration, temp.path);
  assert(failed_registration.vfs.register_attempts == 1 && failed_registration.vfs.links.empty());
}

#include "mod_files.h"

// PS5 regression: after building this test normally, deny even shared-library lstat calls:
// cc -shared -fPIC ps5/tests/mod_files_lstat_denied.c -o /tmp/gta4-deny-lstat.so
// LD_PRELOAD=/tmp/gta4-deny-lstat.so /tmp/gta4-mod-files-test

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct TestFolder {
  fs::path path = fs::temp_directory_path() /
                  ("gta4_mod_files_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFolder() { assert(fs::create_directory(path)); }
  ~TestFolder() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void Write(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "test";
  assert(file.good());
}

std::vector<std::string> Names(const gta4ps5::ModFiles& result) {
  std::vector<std::string> names;
  for (const fs::path& path : result.files) {
    assert(path.is_relative());
    names.push_back(path.generic_string());
  }
  return names;
}

int main() {
  TestFolder temp;
  const fs::path root = temp.path / "mods/game";
  auto result = gta4ps5::ScanModFiles(root);
  assert(result.files.empty() && result.errors.empty());

  Write(root / "common/data/handling.dat");
  Write(root / "common/data/Zones.dat");
  Write(root / "common/data/a.dat");
  Write(root / "common/data/CDimages/script.img");
  Write(root / "pc/models/playerped.rpf");
  const std::vector<std::string> expected = {
      "common/data/a.dat", "common/data/CDimages/script.img", "common/data/handling.dat",
      "common/data/Zones.dat", "pc/models/playerped.rpf"};
  result = gta4ps5::ScanModFiles(root);
  assert(Names(result) == expected && result.errors.empty());
  result = gta4ps5::ScanModFiles(root / "");
  assert(Names(result) == expected && result.errors.empty());
  const auto previous_directory = fs::current_path();
  fs::current_path(temp.path);
  result = gta4ps5::ScanModFiles("mods/game/");
  fs::current_path(previous_directory);
  assert(Names(result) == expected && result.errors.empty());

  for (const char* name : {"menu.ASI", "menu.DLL", "menu.so", "menu.SPRX", "menu.prx",
                           "menu.elf", "menu.exe", "default.XEX", "default.xexp", "AES_KEY.BIN",
                           "manifest.JSON", "install_manifest.json", "gta4_manifest.toml",
                           "README.TXT", "Readme.md", ".install-manifest", ".hidden.dat", ".cache/visible.dat",
                           "common/.hidden/data.dat", "common/data/README.md"})
    Write(root / name);
  Write(temp.path / "outside/escape.dat");
  fs::create_directory_symlink(temp.path / "outside", root / "linked_directory");
  fs::create_symlink(temp.path / "outside/escape.dat", root / "linked_file.dat");
  fs::create_symlink(temp.path / "absent", root / "dangling.dat");
  result = gta4ps5::ScanModFiles(root);
  assert(Names(result) == expected && result.errors.empty());
  fs::create_directory_symlink(root, temp.path / "linked_root");
  result = gta4ps5::ScanModFiles(temp.path / "linked_root");
  assert(result.files.empty() && result.errors.empty());
  result = gta4ps5::ScanModFiles(temp.path / "linked_root/");
  assert(result.files.empty() && result.errors.empty());

  Write(root / "common/data/HANDLING.dat");
  Write(root / "bad:drive/file.dat");
  Write(root / "bad\\directory/file.dat");
  Write(root / "common/data/bad:name.dat");
  Write(root / "common/data/bad\\name.dat");
  result = gta4ps5::ScanModFiles(root);
  const std::vector<std::string> no_duplicates = {
      "common/data/a.dat", "common/data/CDimages/script.img", "common/data/Zones.dat",
      "pc/models/playerped.rpf"};
  assert(Names(result) == no_duplicates);
  assert(result.errors.size() == 5);
  const auto first_errors = result.errors;
  fs::remove(root / "common/data/HANDLING.dat");
  fs::remove(root / "common/data/handling.dat");
  Write(root / "common/data/HANDLING.dat");
  Write(root / "common/data/handling.dat");
  result = gta4ps5::ScanModFiles(root);
  assert(Names(result) == no_duplicates && result.errors == first_errors);

  const fs::path unreadable = root / "unreadable";
  Write(unreadable / "file.dat");
  fs::permissions(unreadable, fs::perms::none);
  std::error_code ec;
  fs::directory_iterator probe(unreadable, ec);
  result = gta4ps5::ScanModFiles(root);
  fs::permissions(unreadable, fs::perms::owner_all);
  // Running as root can bypass permission checks; ordinary host runs must report it.
  if (ec) {
    assert(Names(result) == no_duplicates);
    assert(result.errors.size() == first_errors.size() + 1);
  }

  Write(temp.path / "plain_file");
  result = gta4ps5::ScanModFiles(temp.path / "plain_file");
  assert(result.files.empty() && result.errors.size() == 1);

  const fs::path collisions = temp.path / "directory_collisions";
  Write(collisions / "Common/a.dat");
  Write(collisions / "common/b.dat");
  Write(collisions / "nested/Data/a.dat");
  Write(collisions / "nested/data/b.dat");
  Write(collisions / "nested/model");
  Write(collisions / "nested/Model/file.dat");
  Write(collisions / "linked/Common/a.dat");
  fs::create_directory_symlink(temp.path / "outside", collisions / "linked/common");
  Write(collisions / "other.dat");
  result = gta4ps5::ScanModFiles(collisions);
  assert(Names(result) == std::vector<std::string>{"other.dat"});
  const std::vector<std::string> collision_errors = {
      "common: duplicate case-insensitive mod path", "linked/common: duplicate case-insensitive mod path",
      "nested/data: duplicate case-insensitive mod path", "nested/model: duplicate case-insensitive mod path"};
  assert(result.errors == collision_errors);
  fs::remove_all(collisions / "Common");
  fs::remove_all(collisions / "common");
  Write(collisions / "common/b.dat");
  Write(collisions / "Common/a.dat");
  result = gta4ps5::ScanModFiles(collisions);
  assert(Names(result) == std::vector<std::string>{"other.dat"} && result.errors == collision_errors);
}

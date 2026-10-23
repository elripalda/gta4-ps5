#include "mod_loader.h"

#include <algorithm>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string/utf8.h>

#include "mod_files.h"

REXCVAR_DEFINE_BOOL(ps5_enable_mods, false, "PS5", "Load Xbox 360 file replacements from /app0/mods/game")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace gta4ps5 {

void MountMods(rex::Runtime& runtime, const std::filesystem::path& app) {
  if (!REXCVAR_GET(ps5_enable_mods)) {
    REXLOG_INFO("PS5 mods: disabled");
    return;
  }
  const auto root = app / "mods" / "game";
  const auto scan = ScanModFiles(root);
  for (const auto& error : scan.errors) REXLOG_WARN("PS5 mods: {}", error);
  if (scan.files.empty()) {
    REXLOG_INFO("PS5 mods: no replacement files in '{}'", root.string());
    return;
  }

  constexpr const char* mount = "\\Device\\LibertyRecomp\\Mods";
  // Resolve only indexed files on demand; an eager tree walk would follow ignored symlinks.
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(mount, root, true, false, false, false);
  if (!device->Initialize() || !runtime.file_system()->RegisterDevice(std::move(device))) {
    REXLOG_WARN("PS5 mods: could not mount '{}'", root.string());
    return;
  }

  auto* vfs = runtime.file_system();
  for (const auto& file : scan.files) {
    std::string relative = file.generic_string();
    std::replace(relative.begin(), relative.end(), '/', '\\');
    const std::string target = std::string(mount) + "\\" + relative;
    for (const char* prefix : {"game:\\", "d:\\", "\\Device\\Harddisk0\\Partition1\\"}) {
      const std::string source = std::string(prefix) + relative;
      vfs->UnregisterSymbolicLink(source);
      vfs->RegisterSymbolicLink(source, target);
    }
    // The title update has its own device alias, while living under game/update on disk.
    if (relative.size() > 7 && rex::string::utf8_starts_with_case(relative, "update\\")) {
      for (const char* prefix : {"update:\\", "\\Device\\Harddisk0\\PartitionUpdate\\"}) {
        const std::string source = std::string(prefix) + relative.substr(7);
        vfs->UnregisterSymbolicLink(source);
        vfs->RegisterSymbolicLink(source, target);
      }
    }
    REXLOG_INFO("PS5 mods: game:/{} -> '{}'", file.generic_string(), (root / file).string());
  }
  REXLOG_INFO("PS5 mods: mounted {} replacement file(s); restart with Mods Off to restore the base game",
              scan.files.size());
}

}  // namespace gta4ps5

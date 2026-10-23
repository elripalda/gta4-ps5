# Liberty Recompiled - Mod Support

## PS5 port

The PS5 uses the ReXGlue runtime. The older desktop overlay API and configuration
documented below are separate from the PS5 feature.

1. Use an Xbox 360 mod pack made for the USA release, Title Update 8 and the
   episode you plan to play.
2. Locate the files it replaces under
   `/data/homebrew/PPSA94004/UserData/LibertyRecomp/game/`.
3. Copy the replacements to `/data/homebrew/PPSA94004/mods/game/`, preserving
   their relative paths. For example, a file at
   `UserData/LibertyRecomp/game/<path>/script.img` is replaced by
   `mods/game/<path>/script.img`. Files under `game/update/` need the same
   `update/` prefix under `mods/game/`.
4. Enable **Game → Mods** in the hub and launch the game. Use the menu pack's
   controller shortcut to open it. Quit and restart the app after changing files.

Mods default to Off. The loader mounts replacement files read-only and redirects
game filesystem requests before the guest starts. The loader leaves installed
game files and save files untouched. Startup logs list every replacement and path errors.
To recover from a broken pack, turn Mods off in the hub and restart.

### Script menus

The recompiled game retains the Xbox 360 script VM. A replacement `script.img`
or `script_network.img` can supply modified scripts, but the pack must include
a bootstrap that starts its menu in the intended game mode. Dropping a new
`.sco` into the folder does **not** register or start it. Use one menu pack at
a time; this loader does not merge archives or provide a universal menu shortcut.

Compatibility is experimental until a particular pack has been tested on PS5.
PC `.asi` plugins, ScriptHook DLLs, .NET scripts, PS3 menu binaries and native
Xbox 360 executables require different runtimes or ports. Episode-specific
scripts and hardcoded memory addresses can also make a pack incompatible.
Packs changing the phone script may disable the built-in Quicksave patch for
that script.

### File replacement limits

- Only files opened through the guest filesystem are redirected. A loose file
  does not replace a member inside an IMG/RPF archive. Replace the container
  that the game actually opens when necessary.
- This first version targets the base game's `game:`/`d:` mounts and its
  `update:` files. It does not redirect the separately mounted episode packages
  under `UserData/LibertyRecomp/dlc/`.
- A replacement `xbox360.rpf` takes priority over the generated PlayStation
  button-prompt archive.
- Native executable files, keys, installation metadata, hidden paths, README
  files and symlinks are skipped. Case-insensitive filename collisions are
  skipped and logged. File contents are not validated for game compatibility.
- No game files or third-party menus are included.

To check file discovery on a Linux host without the PS5 SDK:

```sh
g++ -std=c++17 -Wall -Wextra -Werror \
  -Iglue/rexglue-sdk-main/gta4-recomp/src/ps5/game \
  ps5/tests/mod_files_test.cpp \
  glue/rexglue-sdk-main/gta4-recomp/src/ps5/game/mod_files.cpp \
  -o /tmp/gta4-mod-files-test
/tmp/gta4-mod-files-test

# Reproduce the PS5 refusing lstat while retaining cached directory entry types.
cc -shared -fPIC ps5/tests/mod_files_lstat_denied.c -o /tmp/gta4-deny-lstat.so
LD_PRELOAD=/tmp/gta4-deny-lstat.so /tmp/gta4-mod-files-test
```

The [loader integration check](../ps5/tests/mod_loader_test.cpp) and
[VFS regression check](../ps5/tests/vfs_mod_links_test.cpp) include their host
compile commands. They exercise file selection, mounting, and path redirection;
they do not establish compatibility with a particular menu on PS5.

The [save-write regression](../ps5/tests/vfs_save_writes_test.cpp) uses the real
VFS, writable HostPathDevice and POSIX file I/O. Its included Linux build command
checks existing saves, directory-relative creation and overwrite, nested paths,
and read-only mod isolation. Only logging and configuration registration are stubbed.

The PS5 executable still needs the SDK and build steps in [ps5/BUILD.md](../ps5/BUILD.md).

---

## Overview

Liberty Recompiled includes built-in support for mods through a **FusionFix-compatible file overlay system**. This allows modders to override game files by placing modified versions in specific folders, without needing to modify the base game files.

---

## Quick Start

1. Create an `update/` folder next to the LibertyRecomp executable
2. Place your mod files inside, mirroring the game's folder structure
3. Launch the game - mod files will automatically override base files

**Example:**
```
LibertyRecomp/
├── LibertyRecomp.exe (or LibertyRecomp on macOS/Linux)
├── game/                    # Base game files
│   ├── common/
│   ├── xbox360/
│   └── ...
└── update/                  # Mod overlay folder (FusionFix-compatible)
    ├── common/
    │   └── data/
    │       └── handling.dat  # Overrides game/common/data/handling.dat
    └── xbox360/
        └── textures/
            └── my_texture.wtd
```

---

## FusionFix Compatibility

Liberty Recompiled's mod system is designed to be **compatible with FusionFix's folder structure**. If you have FusionFix mods for GTA IV PC, they can work with Liberty Recompiled with minimal or no changes.

### Supported Folder Locations

The mod system scans for overlay folders in this priority order (highest first):

| Priority | Location | Description |
|----------|----------|-------------|
| 100 | `mods/update/` | Highest priority - custom mod folder |
| 50 | `update/` | Standard FusionFix location |
| 40 | `GTAIV.EFLC.FusionFix/update/` | Alternative FusionFix location |
| 30 | `plugins/update/` | Plugins folder |
| 20 | `mods/` | Generic mods folder |

### FusionFix Folder Mapping

FusionFix uses specific subfolder names that are automatically mapped:

| FusionFix Path | Maps To | Description |
|----------------|---------|-------------|
| `update/common/` | `common/` | Common game files |
| `update/pc/` | `pc/` | PC-specific files |
| `update/TLAD/` | `dlc/TLAD/` | The Lost and Damned DLC |
| `update/TBoGT/` | `dlc/TBoGT/` | The Ballad of Gay Tony DLC |
| `update/GTAIV.EFLC.FusionFix/shaders/` | `common/shaders/` | Shader overrides |
| `update/GTAIV.EFLC.FusionFix/textures/` | `xbox360/textures/` | Texture overrides |

---

## Mod Types Supported

### Loose File Overrides

Any file in the overlay folder that matches a game file path will override it:

```
update/common/data/handling.dat     → Overrides handling.dat
update/common/data/vehicles.ide     → Overrides vehicles.ide
update/xbox360/textures/fonts.wtd   → Overrides fonts texture
```

### Data File Mods

Common data files that can be overridden:

| File | Description |
|------|-------------|
| `common/data/handling.dat` | Vehicle handling parameters |
| `common/data/vehicles.ide` | Vehicle definitions |
| `common/data/peds.ide` | Pedestrian definitions |
| `common/data/timecyc.dat` | Time cycle (lighting/weather) |
| `common/data/default.dat` | Default game settings |

### Texture/Model Mods

Texture and model files can be overridden by placing them in the correct path:

```
update/xbox360/textures/  → Texture replacements
update/xbox360/models/    → Model replacements
```

**Note:** Liberty Recompiled uses Xbox 360 format files (`.wtd`, `.wdr`, etc.). PC format files (`.wtx`) may need conversion.

---

## Multiple Mod Folders

You can organize mods into separate folders for easier management:

```
LibertyRecomp/
├── mods/
│   ├── update/           # Primary mod folder (priority 100)
│   │   └── common/
│   └── my_car_mod/       # Additional mod folder
│       └── common/
└── update/               # Secondary mod folder (priority 50)
    └── common/
```

Higher priority folders take precedence. If the same file exists in multiple overlays, the one from the highest priority folder is used.

---

## Adding Custom Overlay Folders

Developers can register additional overlay folders programmatically:

```cpp
#include "kernel/mod_overlay.h"

// Add a custom overlay with specific priority
ModOverlay::AddOverlay("/path/to/my/mod", 75, "My Custom Mod");

// Rebuild the index after adding overlays
ModOverlay::RebuildIndex();
```

---

## Debugging Mods

### Log Output

When mods are loaded, the VFS logs override information:

```
[VFS] ModOverlay initialized: 2 overlays, 15 override files
[VFS] MOD OVERRIDE: 'common/data/handling.dat' -> '/path/to/update/common/data/handling.dat'
```

### Dump Mod Status

Call `ModOverlay::DumpStatus()` to print all registered overlays and overrides:

```
=== MOD OVERLAY STATUS ===
Game Root: /Users/player/LibertyRecomp
Initialized: YES
Total Overlays: 2 (2 enabled)
Override Files: 15
Hits: 42, Misses: 128
Registered Overlays:
  [ON] mods/update (FusionFix) (priority=100, path=/Users/player/LibertyRecomp/mods/update)
  [ON] update (priority=50, path=/Users/player/LibertyRecomp/update)
```

---

## Limitations

### Xbox 360 vs PC Formats

Liberty Recompiled uses Xbox 360 game files. Some PC mod formats may not be directly compatible:

| PC Format | Xbox 360 Format | Notes |
|-----------|-----------------|-------|
| `.wtx` | `.wtd` | Texture containers differ |
| `.wft` | `.wft` | Often compatible |
| `.wad` | `.wad` | May need conversion |

### Not Yet Supported

The following FusionFix features are not yet implemented:

- **IMG Loader**: Loading modified `.img` archives
- **RPF Loader**: Loading modified `.rpf` archives  
- **ASI Plugins**: Native plugin loading
- **Script Mods**: SCO/Script modifications

These may be added in future updates.

---

## Configuration

Mod support can be configured in the game's config file:

```toml
[Mods]
# Enable/disable mod loading
EnableMods = true

# Additional mod folders (semicolon-separated)
ModFolders = "mods/update;update;my_mods"

# Log mod file access (for debugging)
LogModAccess = false
```

---

## Creating Compatible Mods

To create mods compatible with Liberty Recompiled:

1. **Use correct folder structure**: Mirror the game's file hierarchy
2. **Use Xbox 360 formats**: When possible, use `.wtd`, `.wdr`, etc.
3. **Test with logging enabled**: Check that your files are being loaded
4. **Consider priority**: Place most important overrides in `mods/update/`

### Example: Vehicle Handling Mod

```
update/
└── common/
    └── data/
        └── handling.dat   # Your modified handling file
```

### Example: Texture Replacement

```
update/
└── xbox360/
    └── textures/
        └── vehicles/
            └── infernus.wtd   # Your texture file
```

---

## API Reference

### ModOverlay Namespace

| Function | Description |
|----------|-------------|
| `Initialize(gameRoot)` | Initialize the mod system |
| `IsInitialized()` | Check if mod system is ready |
| `AddOverlay(path, priority, name)` | Register a mod folder |
| `RemoveOverlay(path)` | Remove a mod folder |
| `SetOverlayEnabled(path, enabled)` | Enable/disable an overlay |
| `RebuildIndex()` | Rebuild file index after changes |
| `Resolve(normalizedPath)` | Get override path for a game file |
| `HasOverride(normalizedPath)` | Check if override exists |
| `GetStats()` | Get usage statistics |
| `DumpStatus()` | Print debug information |

---

## Troubleshooting

### Mod files not loading

1. Check that the file path matches exactly (case-insensitive)
2. Verify the overlay folder is being detected (check logs)
3. Ensure the file format is correct for Liberty Recompiled

### Game crashes with mods

1. Remove mods and verify base game works
2. Add mods one at a time to identify the problematic one
3. Check that mod files aren't corrupted

### Wrong version of file loading

1. Check overlay priorities - higher priority wins
2. Use `ModOverlay::DumpStatus()` to see which overlay provides each file

---

## See Also

- [INSTALLATION_ARCHITECTURE.md](INSTALLATION_ARCHITECTURE.md) - Installation flow
- [RPF_EXTRACTION_DESIGN.md](RPF_EXTRACTION_DESIGN.md) - How game files are extracted
- [FusionFix GitHub](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) - Original FusionFix project

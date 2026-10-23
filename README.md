# GTA IV for PS5

**Grand Theft Auto IV, The Lost and Damned and The Ballad of Gay Tony (Xbox 360) running natively on a jailbroken PS5**, in 4K with 120 Hz and VRR support, PlayStation buttons and DualSense features.

> ### Built on Liberty Recompiled by OZORDI
> This port exists because of **[Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)**, OZORDI's native recompilation of GTA IV, and **[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)**, vaduur's fork that this port starts from. The recompiled game, its renderer and most of what it does come from their work, with **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** underneath. This release takes that work to the PS5.
>
> All the credit goes to them. Go star the original projects and support them.

## Get it on Recomp Store
The easiest way to play is **[Recomp Store](https://recompstore.com)**. It installs the app, downloads Title Update 8 for you and keeps it up to date.

## What's in it
- GTA IV and both Episodes from Liberty City, recompiled to run natively on the PS5
- 4K with a 60 FPS limit. Most of the city holds 60, the busiest streets can dip into the high 40s and 50s
- 120 Hz and VRR support, plus a 40 FPS mode for 120 Hz TVs
- PlayStation button prompts
- DualSense light bar (police lights when you're wanted), adaptive triggers and extra rumble
- A hub to pick GTA IV or either episode, each with its own look and menu music
- Installs the game and the episodes from your own files, imports your Xbox 360 saves, backs up and restores saves, and has trophies
- Display presets: Recommended, Performance and Quality, all at 60 FPS
- Graphics settings: resolution, draw distance, city population, anti-aliasing, texture filtering, shadows, reflections, motion blur, film grain and FusionFix's enhanced effects
- Centered car camera, and how quickly the car camera returns and follows
- An FPS counter (simple or detailed), off by default
- English, French, German, Spanish and Italian

## What you need
- A jailbroken PS5 that runs homebrew apps from `/data/homebrew/`
- Your **own** copy of GTA IV for the **USA Xbox 360** as an `.iso`, and its Title Update 8 (Recomp Store gets it for you). No game files are included.
- For the episodes, their package from your Xbox 360
- About 17 GB free

> [!TIP]
> ### 💙 Enjoying the port?
> If you've gotten some enjoyment out of this project, donations and love are always appreciated. Every bit helps keep these ports growing. Thank you!
>
> [![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/ripalda)

## Install by hand
1. Download `PPSA94004.zip` from [Releases](../../releases) and copy the `PPSA94004` folder to `/data/homebrew/` on your PS5.
2. Put your disc image and Title Update 8 in its `install` folder:

```
/data/homebrew/PPSA94004/install/Grand Theft Auto IV (USA).iso
/data/homebrew/PPSA94004/install/TU_1A581VI_000000K000000.0000000000205
```

3. Launch it and press X on Home. With a payload loader running (etaHEN, elfldr or any other) the install runs at the drive's full speed. Without one it still works, just much slower. You can delete both files afterwards.
4. For the episodes, copy their package from your 360's drive (`Content/0000000000000000/545407F2/00000002`) into the same `install` folder and install them from the Episodes page.

To update, copy the new `PPSA94004` folder over the old one. The installed game, your saves and your settings stay.

## Xbox 360 saves
Copy your saves off the 360 (`Content/<your profile>/545407F2/00000001/`) into `/data/homebrew/PPSA94004/import/`, then pick Import on the hub's Saves page.

## Mods and script menus

Enable **Game → Mods** in the hub to load replacement files from
`/data/homebrew/PPSA94004/mods/game/`. Mirror each file's path under the installed
`UserData/LibertyRecomp/game/` folder. Quit and restart the app after changing files.
Turn Mods off and restart to return to the original game.

Xbox 360 script-menu archive packs with their own loader/bootstrap are the
initial target; individual menus still need testing on PS5. Loose `.sco` files
do not start automatically, and PC `.asi` plugins and ScriptHook DLLs cannot
run here. See [PS5 mod setup and limitations](docs/MOD_SUPPORT.md#ps5-port).

## The hub
| Button | Does |
|---|---|
| L1 / R1 | Switch pages |
| Left / Right | Pick a game on Home, change a setting |
| Cross | Play, or select |
| Options | Play from any page |
| Triangle | Reset the settings |
| Circle | Back to Home |

## Known issues
- The busiest streets can drop below 60 FPS at 4K.
- Only the USA disc and Title Update 8 are supported.

## Reporting a problem
Report bugs on the **[RIPALDA Discord](https://discord.gg/GgDE5Vynyu)**. Send the newest file from `/data/homebrew/PPSA94004/logs/`, with what you were doing and your settings.

## Building from source
The whole source is in this repository. Its root is [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid) as vaduur made it (his README is [README-LibertyRecompAndroid.md](README-LibertyRecompAndroid.md), Liberty Recompiled's is [README_UPSTREAM.md](README_UPSTREAM.md)), with the PS5 changes marked `[ps5 port]`. The PS5 port itself is in [`ps5/`](ps5) and `glue/rexglue-sdk-main/gta4-recomp/src/ps5/`: the hub, the PS5 start-up, the trophies, the build and the packaging. [`patches/liberty-ps5.patch`](patches/liberty-ps5.patch) has every change against LibertyRecompAndroid in one file.

Run `./setup.sh` to fetch the third-party code (git submodules) and patch it, then follow [ps5/BUILD.md](ps5/BUILD.md). You don't need the game to build it, and nothing from the game is stored here. The hub's Helvetica Neue Heavy can't be shared, so put your own `HelveticaNeue-Heavy.ttf` in `ps5/assets/fonts/` (without it the tabs use the hub's usual type).

A few big reference copies in LibertyRecompAndroid's `tools/` aren't used to build and are left out here: Ghidra, FusionFix's source, the FidelityFX SDK and a copy of Xenia. They're in LibertyRecompAndroid and their own projects.

## Credits
- **[OZORDI](https://github.com/OZORDI)**: [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp), the GTA IV recompilation this is built on, with its contributors
- **[vaduur](https://github.com/vaduur)**: [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid), the fork this port starts from
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**: the static recompiler and runtime, which builds on [Xenia](https://github.com/xenia-project/xenia) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
- **[ThirteenAG](https://github.com/ThirteenAG)**: [FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix), whose shaders are the Enhanced Effects option and whose camera options the car camera settings follow
- **[elripalda](https://github.com/elripalda)**: the PS5 port, the hub, the PlayStation buttons and the art
- **[mihawk-99](https://github.com/mihawk-99)**: [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), the PS5 Vulkan driver
- **[Nazky](https://github.com/Nazky)**: [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle), the PS5 Vulkan app setup this builds on
- **[ps5-payload-dev](https://github.com/ps5-payload-dev)**: the PS5 payload SDK
- AMD FidelityFX, [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [FFmpeg](https://ffmpeg.org), [stb](https://github.com/nothings/stb), [dr_libs](https://github.com/mackron/dr_libs), and the TeX Gyre Heros, Oswald and Inter fonts
- Thanks to everyone who played the betas

## License
Liberty Recompiled, LibertyRecompAndroid and this port are under the [GPL-3.0](LICENSE). ReXGlue keeps its own BSD-style license. None of these cover the game, its artwork or its music.

*An unofficial fan project, not affiliated with or endorsed by Rockstar Games, Take-Two Interactive, Microsoft or Sony. No game files are included.*

# Building GTA IV for PS5

Built on Linux (Ubuntu 24.04), like the other ports.

## What you need

- clang 19, lld 19, LLVM 19 tools (`llvm-config-19`), CMake 3.25+, Ninja, Python 3, git
- Nazky's [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle), built once with `tools/build.sh`
  (the PS5 payload SDK, RADV, the app CRT and the signer)
- DXC 1.9 for Linux (1.8 crashes on the Bink video shaders):
  `linux_dxc_2025_07_14.x86_64.tar.gz` from DirectXShaderCompiler v1.8.2505.1

No game files are needed to build: the recompiled code is already in the repo.

## 1. Source

```
git clone https://github.com/elripalda/gta4-ps5
cd gta4-ps5
./setup.sh
```

`setup.sh` fetches the third-party code the build uses (git submodules) and
applies the PS5's patches to FFmpeg and libmspack.

## 2. Build

```
SDK=~/ps5-plume-triangle/.deps/native/ps5-payload-sdk
DXC=/path/to/dxc
cmake -S ps5 -B out/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ps5/cmake/ps5-toolchain.cmake -DPS5_PAYLOAD_SDK=$SDK \
  -DGTA4_NATIVE_DXC=$DXC/bin/dxc -DGTA4_NATIVE_DXC_LIBRARY_PATH=$DXC/lib \
  -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
  -DSDL_OSS=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
  -DSDL_SNDIO=OFF -DSDL_HIDAPI=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF \
  -DSDL_VULKAN=OFF -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON -DSDL_OFFSCREEN=OFF \
  -DREXGLUE_ENABLE_FIDELITYFX=ON -DGTA4_NATIVE_SPIRV_VAL=$(which spirv-val) \
  -DGTA4_NATIVE_GLSLANG_VALIDATOR=$(which glslangValidator) \
  -DGTA4_TEMPORAL_ZSTD_LIBRARY=/usr/lib/x86_64-linux-gnu/libzstd.so.1
ninja -C out/ps5 gta4

ps5/tools/ps5_package.sh out/ps5 ~/ps5-plume-triangle
```

The app ends up in `out/ps5/ps5-app/PPSA94004`. Copy it to `/data/homebrew/` on the PS5.

The FidelityFX lines add FSR 3 (the hub's Frame Generation option). They need Wine
(`apt install wine64`) for AMD's shader compiler, plus spirv-tools and glslang. The
first build compiles about 2,700 motion-vector shader variants, which takes a while.
Leave out `-DREXGLUE_ENABLE_FIDELITYFX=ON` and
`-DGTA4_TEMPORAL_ZSTD_LIBRARY=...` to build without FSR 3. Keep the
`spirv-val` and `glslangValidator` options: the regular shader cache also needs them.

The hub draws its tabs in Helvetica Neue Heavy, as the episodes' menus do.
Its licence doesn't allow sharing it, so it isn't in this repo: put your own
`HelveticaNeue-Heavy.ttf` in `ps5/assets/fonts/` before building, or the tabs
use the hub's usual type.


## On the PS5

- `install/`: your GTA IV disc image (USA, .iso) and Title Update 8. The game installs itself from them the first time.
- `import/`: Xbox 360 save files, imported from the hub's Saves page.
- `logs/`: the log, and frame time captures when they're on.
- `mods/game/`: optional Xbox 360 replacement files, enabled by **Game → Mods**
  in the hub. See [mod setup and limitations](../docs/MOD_SUPPORT.md#ps5-port).

## Where things are

| | |
|---|---|
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/hub/` | the hub |
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/game/` | trophies, the FPS counter, 360 save import, sound |
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/gta4_ps5_app.cpp` | where the game and the hub meet |
| `ps5/payload/installer/` | the installer payload: the app sends it to the payload loader on port 9021 to install the game at full speed (built by `ps5_package.sh`) |
| `ps5/package/` | home screen icon, hub art, each game's menu music and the menu sounds, the button pictures |
| `ps5/tools/` | packaging, the button pictures, the hub preview |

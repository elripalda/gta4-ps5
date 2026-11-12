# Building GTA IV for PS5

Built on Linux (Ubuntu 24.04), like the other ports.

## What you need

- clang 19, lld 19, CMake 3.25+, Ninja, Python 3, git
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
git clone https://github.com/mihawk-99/PS5_PayloadSDK.git /path/to/f02-sdk-source
git -C /path/to/f02-sdk-source checkout 95c08f27386fc698f6bbe21dde3030140a41d10b
python3 ps5/tools/prepare_f02_sdk.py --source /path/to/f02-sdk-source \
  --base-sdk ~/ps5-plume-triangle/.deps/native/ps5-payload-sdk \
  --output /path/to/fresh-f02-sdk
export LLVM_CONFIG=$(command -v llvm-config-19)
export PS5_PAYLOAD_SDK=/path/to/fresh-f02-sdk
SDK=$PS5_PAYLOAD_SDK
DXC=/path/to/dxc
cmake -S ps5 -B out/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/ps5/cmake/ps5-toolchain.cmake -DPS5_PAYLOAD_SDK=$SDK \
  -DPS5_ZLIB_ROOT=~/ps5-plume-triangle/.deps/native/zlib/root/usr \
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

The SDK preparation copies the existing toolchain/runtime into a fresh directory,
archives pristine SDK source at the pin, applies the tracked lifecycle patch, and
rebuilds `libps5platform.a` and its headers. Configure and packaging verify the
source/header/archive hashes in its receipt; an old patched SDK is not accepted.
The base SDK and its source checkout are never edited. Optional `--cc`, `--ar`,
`--nm` and `--llvm-config` arguments select an already installed LLVM toolchain.

The app ends up in `out/ps5/ps5-app/PPSA94004`. Copy it to `/data/homebrew/` on the PS5.

The FidelityFX lines add FSR 3 (the hub's Frame Generation option). They need Wine
(`apt install wine64`) for AMD's shader compiler, plus spirv-tools and glslang. The
first build compiles about 2,700 motion-vector shader variants, which takes a while.
Leave those four lines out to build without FSR 3.

The hub draws its tabs in Helvetica Neue Heavy, as the episodes' menus do.
Its licence doesn't allow sharing it, so it isn't in this repo: put your own
`HelveticaNeue-Heavy.ttf` in `ps5/assets/fonts/` before building, or the tabs
use the hub's usual type.


## Host lifecycle gates

```
python3 ps5/tools/run_f02_host_gates.py --out /path/to/new-results \
  --sdk-source "$PS5_PAYLOAD_SDK/_f02_source"
cmake -S ps5/tests -B out/f02-host -G Ninja \
  -DF02_SDK_SOURCE="$PS5_PAYLOAD_SDK/_f02_source"
cmake --build out/f02-host
ctest --test-dir out/f02-host --output-on-failure
```

The matrix runs GCC and Clang, plain and ASan/UBSan, against the current source.
These gates do not establish PS5 gameplay acceptance. An extracted integration
is different from earlier tested executables and needs its own gameplay evidence.

## On the PS5

- `install/`: your GTA IV disc image (USA, .iso) and Title Update 8. The game installs itself from them the first time.
- `import/`: Xbox 360 save files, imported from the hub's Saves page.
- `logs/`: the log, and frame time captures when they're on.

## Where things are

| | |
|---|---|
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/hub/` | the hub |
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/game/` | trophies, the FPS counter, 360 save import, sound |
| `glue/rexglue-sdk-main/gta4-recomp/src/ps5/gta4_ps5_app.cpp` | where the game and the hub meet |
| `ps5/payload/installer/` | the installer payload: the app sends it to the payload loader on port 9021 to install the game at full speed (built by `ps5_package.sh`) |
| `ps5/package/` | home screen icon, hub art, each game's menu music and the menu sounds, the button pictures |
| `ps5/tools/` | packaging, the button pictures, the hub preview |

#!/usr/bin/env bash
# GTA IV for PS5: link the PS5 build and package it as a PS5 homebrew app
# folder (eboot.bin, sce_module/libc.prx, sce_sys, Resources).
#
#   ps5/tools/ps5_package.sh <cmake build dir> <ps5-plume-triangle checkout> [title id]
#
# The link and packaging reuse the PS5 tooling of Nazky's ps5-plume-triangle
# / UnleashedRecomp-Prospero (mihawk-99's RADV link recipe, app CRT, libc.prx
# builder and FSELF signer): build that project once first (its build/ holds
# app_crt.o, app_cpp_runtime.o, the libSceAgc stubs and ps5-native-tool).
# No game files are packaged: they go in the app's Assets folder on the PS5
# (ps5/package/Assets/README.txt).
set -euo pipefail

build=$(realpath "${1:?cmake build dir}")
tri=$(realpath "${2:?ps5-plume-triangle checkout}")
title_id=${3:-PPSA94004}
ps5dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

sdk_root=${PS5_PAYLOAD_SDK:-$tri/.deps/native/ps5-payload-sdk}
radv_archive=${RADV_ARCHIVE:-$tri/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
tool=$tri/build/host/ps5-native-tool
for f in "$sdk_root/bin/prospero-lld" "$radv_archive" "$tool" "$tri/build/obj/app_crt.o" \
         "$tri/build/obj/app_cpp_runtime.o" "$tri/build/stubs/libSceAgc.so" "$tri/runtime/libc.prx"; do
    [[ -e $f ]] || { echo "missing $f (build ps5-plume-triangle first)" >&2; exit 2; }
done

# Staging root for radv_link_recipe: it reads tooling/ and vendor/ and writes build/.
stage=$build/ps5-link
mkdir -p "$stage/build"
ln -sfn "$tri/tooling" "$stage/tooling"
ln -sfn "$tri/vendor" "$stage/vendor"

# The objects and static libraries CMake links into gta4, in its order.
link_cmd=$(ninja -C "$build" -t commands gta4 | tail -1)
mapfile -t inputs < <(cd "$build" && for w in $link_cmd; do
    case $w in
        */libz.a) ;;  # (RADV's archive has zlib in it)
        *.o|*.a) [[ $w = /* ]] && echo "$w" || echo "$build/$w" ;;
    esac
done)
echo "==> [link] ${#inputs[@]} objects and libraries from CMake"

export PS5_CLANG=${PS5_CLANG:-$(command -v clang-19 || command -v clang)}
source "$tri/tools/radv-link.sh"
radv_link_recipe "$stage" "$sdk_root" "$radv_archive"
# The recipe's emulated TLS with a thread-exit fix (ps5_emutls.c), compiled
# over the recipe's object, which the link uses.
"$PS5_CLANG" --target=x86_64-sie-ps5 -fPIC -fno-plt -fno-stack-protector \
    -isysroot "$sdk_root" -isystem "$sdk_root/target/include" \
    -O2 -c "$ps5dir/tools/ps5_emutls.c" -o "$stage/build/ps5-emutls-cxa.o"

# stat("") / lstat("") as on Linux (ps5_stat.c, through --wrap).
"$sdk_root/bin/prospero-clang" -c -O2 -fno-visibility-from-dllstorageclass -o "$stage/build/ps5_stat.o" "$ps5dir/tools/ps5_stat.c"

link_pie() {  # extra objects, if any
    "$sdk_root/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
        --version-script "$tri/tooling/native/app-symbols.map" --exclude-libs=ALL \
        --wrap=stat --wrap=lstat \
        --wrap=sceVideoOutIsOutputSupported --wrap=sceVideoOutConfigureOutput --wrap=sceVideoOutSetFlipRate \
        -e _start -o "$stage/build/llvm-pie.elf" \
        "$tri/build/obj/app_crt.o" "$tri/build/obj/app_cpp_runtime.o" "$stage/build/ps5_stat.o" "$@" \
        --start-group "${inputs[@]}" --end-group \
        "$tri/build/stubs/libSceAgc.so" "$tri/build/stubs/libSceAgcDriver.so" \
        "${radv_link_inputs[@]}" \
        --as-needed "$sdk_root"/target/lib/*.so
}
make_eboot() {
    "$tool" link --in "$stage/build/llvm-pie.elf" --out "$stage/build/eboot.elf" \
        --stub-dir "$sdk_root/target/lib" --stub "$tri/build/stubs/libSceAgc.so" \
        --stub "$tri/build/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
        --companion-sdk 0x08050001 --file-name eboot.elf
}

echo "==> [link] GTA IV + RADV into a PS5 PIE ELF"
link_pie

echo "==> [package] eboot.bin for $title_id"
if ! make_eboot 2> "$stage/build/eboot.err"; then
    cat "$stage/build/eboot.err" >&2
    # The tool appends the process parameters right after the image's RELRO
    # content, inside its last page; when that content happens to end near
    # the page boundary there is no room. Link again with half a page of
    # RELRO padding (ps5_relro_pad.c), which moves the end well clear of it.
    grep -q "no room for PS5 process parameters" "$stage/build/eboot.err" || exit 1
    echo "==> [link] again with 2 KB of RELRO padding (room for the process parameters)"
    "$sdk_root/bin/prospero-clang" -c -O2 -fPIC -fno-visibility-from-dllstorageclass \
        -o "$stage/build/ps5_relro_pad.o" "$ps5dir/tools/ps5_relro_pad.c"
    link_pie "$stage/build/ps5_relro_pad.o"
    make_eboot
fi
app=$build/ps5-app/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$stage/build/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$tri/runtime/libc.prx" "$app/sce_module/libc.prx"
# attribute3 262208 (0x40 | 0x40000): 120 Hz output, with VRR left to the
# system, as retail titles declare it. RADV asks VideoOut for the 120 Hz mode
# when it's declared; the game only takes it when the hub's Refresh Rate is
# 120 (src/ps5/game/ps5_display.cpp).
cat > "$app/sce_sys/param.json" <<EOF
{
  "ageLevel": { "default": 0 },
  "applicationCategoryType": 0,
  "applicationDrmType": "free",
  "attribute": 0,
  "attribute2": 0,
  "attribute3": 262208,
  "conceptId": "${title_id:4}",
  "contentBadgeType": 1,
  "contentId": "UP9000-${title_id}_00-GTAIVPS500000000",
  "contentVersion": "01.000.000",
  "downloadDataSize": 256,
  "gameIntent": { "permittedIntents": [ { "intentType": "launchActivity" } ] },
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": { "titleName": "Grand Theft Auto IV" }
  },
  "masterVersion": "01.00",
  "pubtools": { "creationDate": "$(date -u +%Y-%m-%d) 00:00:00", "loudnessSnd0": "-28.00", "toolVersion": "2.00" },
  "requiredSystemSoftwareVersion": "0x0000000000000000",
  "sdkVersion": "0x0000000000000000",
  "titleId": "${title_id}",
  "versionFileUri": ""
}
EOF
# The home screen icon, the hub (art, button pictures, music), the import
# folder for Xbox 360 saves, and the title resources the game reads from
# /app0/Resources (fonts, PlayStation button prompts, the RPF key).
repo=$(cd -- "$ps5dir/.." && pwd)
cp -r "$ps5dir/package/sce_sys/." "$app/sce_sys/"
cp -r "$ps5dir/package/hub" "$app/hub"
mkdir -p "$app/import" "$app/hub/music"
cp "$ps5dir/package/import/README.txt" "$app/import/README.txt"
mkdir -p "$app/mods/game"
cp "$ps5dir/package/mods/README.txt" "$app/mods/README.txt"
mkdir -p "$app/Resources/button_prompts" "$app/install"
cp -r "$repo/LibertyRecompLib/font_atlases" "$app/Resources/"
cp -r "$repo/LibertyRecompLib/private/button_prompts/." "$app/Resources/button_prompts/"
[[ -f "$repo/LibertyRecompLib/aes_key.bin" ]] && cp "$repo/LibertyRecompLib/aes_key.bin" "$app/Resources/"
cp "$ps5dir/package/install/README.txt" "$app/install/README.txt"
# The licences of the fonts built into the app (the hub's type and the overlay's).
mkdir -p "$app/licenses"
cp "$ps5dir"/assets/fonts/*.txt "$app/licenses/"
# Pipelines a long session built (the renderer's InitializeNativePipelineCache
# takes them in), so first sights of a car or a street don't stall the frame.
if [[ -d $ps5dir/package/shader_cache_seed ]]; then
    cp -r "$ps5dir/package/shader_cache_seed" "$app/shader_cache_seed"
fi
# FSR 3's motion-vector shaders (builds with FidelityFX on): the renderer looks
# for them next to the executable.
temporal=$build/glue/gta4-recomp/shaders/vulkan-temporal.bin
if [[ -f $temporal ]]; then
    mkdir -p "$app/shaders"
    cp "$temporal" "$app/shaders/vulkan-temporal.bin"
fi
# The installer payload (ps5/payload/installer): the app sends it to the
# console's payload loader on port 9021 to install the game at the drive's
# full speed.
zlib=${ZLIB:-$tri/.deps/native/zlib/root/usr}
make -s -C "$ps5dir/payload/installer" PS5_PAYLOAD_SDK="$sdk_root" ZLIB="$zlib"
mkdir -p "$app/payloads"
cp "$ps5dir/payload/installer/gta4-ps5-installer.elf" "$app/payloads/"
"$tool" self --inspect --file "$app/eboot.bin" | tail -3
echo "==> $app ($(stat -c %s "$app/eboot.bin") bytes)"

# Frontend controller diagram patcher

Desktop-only tooling for the GTA IV and Episodes from Liberty City **Controls**
screen. It replaces only the `pack:/controller.dds` surface inside a
user-owned `frontend.xtd` dictionary and leaves every other texture, label and
button mapping byte-identical.

This directory contains no game data. You must supply your own installed game
files. A redistributable 512×512 DXT5 controller diagram and its generator are
provided in [`artwork/`](artwork/README.md); the third-party artwork used for
the byte-pinned vectors is not distributed.

## What it supports

| Game | Mechanism | Installed by this tool |
| --- | --- | --- |
| GTA IV (base) | Runtime shadow RPF inside the game's patched `xbox360.rpf` | No. The game performs this at runtime; the CLI can only convert a loose base dictionary for inspection. |
| TLAD | Loose `dlc/TLAD/xbox360/textures/frontend.xtd` | Yes, with `install` / `rollback` |
| TBOGT | Loose `dlc/TBOGT/xbox360/textures/frontend.xtd` | Yes, with `install` / `rollback` |

The converter replaces the controller surface, re-tiles it for the Xbox GPU
layout, re-emits the dictionary as LZX and decodes the result again to verify
the round trip before returning.

## Build

Configure the repository normally so the dependency patches are applied, then
build the tool target:

```sh
cmake -S . -B build
cmake --build build --target gta4_frontend_controller_patch
```

The executable is written under `build/tools/frontend_controller_patch/`.

For a standalone build against a patched libmspack checkout:

```sh
cmake -S tools/frontend_controller_patch -B build-frontend-tool \
  -DF01_LIBMSPACK_DIR=/path/to/libmspack/mspack
cmake --build build-frontend-tool
```

## Input requirements

- A stock, user-owned `frontend.xtd` from your installation.
- `controller_512.dds`: 512×512, single mip, DXT5, 262,144-byte payload.
- Alternatively use `artwork/dualsense_controller_512.dds`: it meets the same
  format requirements but is not the byte-pinned artwork.
- The verified stock dictionaries are pinned by SHA-256; unknown files are
  refused unless you pass `--allow-unknown-input` (see `known`).

Run `gta4_frontend_controller_patch known` for the verified hashes and the PS5
loose-file paths.

## Convert

```sh
gta4_frontend_controller_patch convert \
  --game tlad \
  --input /path/to/user-owned/frontend.xtd \
  --dds /path/to/controller_512.dds \
  --output /path/to/tlad_frontend_clean.xtd
```

With the verified stock dictionary and verified artwork, the tool checks the
output against the pinned SHA-256 and refuses to write on a mismatch. With
custom artwork the output is structurally validated and round-trip verified,
but not byte-pinned.

The output path must differ from the input path; retail dictionaries are never
overwritten by `convert`.

## Install (TLAD / TBOGT only)

`install` is a dry run unless `--apply` is present:

```sh
gta4_frontend_controller_patch install \
  --game tlad \
  --source /path/to/tlad_frontend_clean.xtd \
  --target /data/homebrew/PPSA94004/UserData/LibertyRecomp/dlc/TLAD/xbox360/textures/frontend.xtd \
  --backup-dir /data/homebrew/PPSA94004/UserData/LibertyRecomp/backup/f01-controller
```

Add `--apply` to perform the install. The tool:

1. Verifies the target is the verified stock or verified clean dictionary.
2. Writes a timestamped backup and verifies it.
3. Writes the new file to a temporary path, verifies it, and replaces the
   target.
4. Verifies the installed hash; on any failure it restores the backup.

Use `--allow-unverified-source` for a custom conversion and
`--allow-unknown-target` only when you have your own backup of an unexpected
target file.

Close the game before replacing files. The path above is the PS5 install path;
on a desktop, point `--target` at a copy of your dictionary.

## Rollback

```sh
gta4_frontend_controller_patch rollback \
  --game tlad \
  --target /data/homebrew/PPSA94004/UserData/LibertyRecomp/dlc/TLAD/xbox360/textures/frontend.xtd \
  --backup /data/homebrew/PPSA94004/UserData/LibertyRecomp/backup/f01-controller/frontend.xtd.f01-backup-20261108T000000Z.bak
```

`rollback` is also a dry run without `--apply`. It preserves the current file
as a `.pre-rollback-*.bak` before restoring the verified stock backup and never
deletes the backup.

## Verified vectors

With the verified 512×512 artwork, the production transformer reproduces these
accepted outputs byte-for-byte:

| Game | Stock input SHA-256 | Clean output SHA-256 | Size |
| --- | --- | --- | ---: |
| GTA IV | `265d90312d54c909de1160154f93a79b2fba487bc9b6044d556da07d9607082e` | `3d6a1658fd80ed5307837445f00734c060efb51f3fdd5e2acf5b28aac0a0c620` | 1,430,448 |
| TLAD | `adaa899fa1f72f69f1e3aee2a23efe7b046b6f7f5ae857f84c4a0e96e754b254` | `85ae8912d55f98be20ec0e213e9f4a6e09bc0c8f956d6187091b18add7c739b8` | 1,430,448 |
| TBOGT | `24ac4d7f8009b926336614b0a897927588ca6a91a70748381c7460d728e2367d` | `19006797766794618bdbacb2f657f39f1271294c62e46ddb76ea5fbae66b9b35` | 1,446,853 |

The verified artwork hash is
`f844b50a5101d147ecd9b2b6b7933eb8313a3473cceee726ec0fefbb4c71048b`. That DDS
is **not** distributed here; its redistribution rights are unresolved. It is a
512×512 derivative of the upstream FusionFix
`textures/pc/textures/buttons_360.wtd/ps5_controller.dds`
(SHA-256 `26aea8c483a87f1a476659fd4da7ae47bc5162f982019bc2bc801cbbdd524e18`).
See [`artwork/README.md`](artwork/README.md) for the upstream link, the bundled
GPL-3.0-or-later alternative and its validation hashes.

## Limitations

- Only `pack:/controller.dds` is replaced. GXT text, button mappings, texture
  slots and all unrelated dictionary resources are preserved.
- Only 512×512, single-mip DXT5 input is accepted. Other DDS layouts are
  rejected with an error.
- The dictionary transform requires the repository's patched libmspack XMem
  handling; a stock system libmspack may decode CAB padding incorrectly.
- The base-game path is performed by the game at runtime. This CLI does not
  install base-game files.
- The tool does not launch the game, contact a console, or modify saves,
  trophies, firmware or the eboot.
- Unknown game builds are rejected by default; `--allow-unknown-input` is an
  explicit opt-in and forfeits the byte-exact guarantee.

## Licensing

- No Rockstar game data, generated archives, keys, or unresolved third-party
  artwork are included in this directory.
- The verified artwork used for the accepted build is a FusionFix-derived
  asset (GPL-3.0, original artist unverified) and is not redistributed.
- `artwork/` contains an independently authored diagram and generator, licensed
  GPL-3.0-or-later. See [`artwork/README.md`](artwork/README.md).
- This code is part of the GPL-3.0 repository.

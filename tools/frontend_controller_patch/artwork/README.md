# DualSense controller diagram artwork

This directory carries the **redistributable** artwork used to exercise the
frontend controller patcher for GTA IV, TLAD and TBOGT.

It contains no Rockstar game data, no extracted archives and no third-party
work whose redistribution rights are unresolved.

## Files

| File | Format | SHA-256 |
| --- | --- | --- |
| `dualsense_controller.svg` | Editable 512×512 SVG | `43bdf6947e63d1494c07b72b4907d0b65ae5643d857862e2a46e5f96b60100cc` |
| `dualsense_controller.png` | 512×512 RGBA PNG preview | `d6b2fae5b6ccdc8a193ea11853a47163f026fe3610ad59318fb8543f98da9a9c` |
| `dualsense_controller_512.dds` | 512×512 DXT5, 0 mips, 262,272 bytes | `a476b945aae0cbf74adec518dc7fed7b075ba7718ec3467ef027d91382d04bb3` |
| `dualsense_geometry.py` | Geometry source of truth | `e5b087d1f85c2f3a627b9a9aedb70d8b0d12835229b1bcb74b0e4354c392d52d` |
| `render_dds.py` | Deterministic SVG/PNG/DDS generator | `c18037f66f3c985e0a51acf32970fffedbc30bbc0d3cd1391c8072b3eff14118` |

`SHA256SUMS` repeats the file hashes for verification.

## Why two artworks are involved

The tests recorded in the top-level README were produced with a 512×512 DXT5
derivative of **ThirteenAG/GTAIV.EFLC.FusionFix**'s `ps5_controller.dds`:

- Upstream file:
  <https://raw.githubusercontent.com/ThirteenAG/GTAIV.EFLC.FusionFix/master/textures/pc/textures/buttons_360.wtd/ps5_controller.dds>
- Upstream SHA-256: `26aea8c483a87f1a476659fd4da7ae47bc5162f982019bc2bc801cbbdd524e18`
  (1024×1024)
- Derived 512×512 DXT5 used by the pinned vectors:
  `f844b50a5101d147ecd9b2b6b7933eb8313a3473cceee726ec0fefbb4c71048b`

That derivative is **not** committed here. FusionFix is GPL-3.0, but the
licence is repository-wide with no per-asset grant or artist attribution, and
the image reproduces the retail Controls-screen leader-line layout, so
redistribution permission is not established. Fetch it from the upstream link
above and use it locally if you need the byte-pinned vectors.

The files in this directory are a **separately authored** diagram:

- Geometry is defined numerically in `dualsense_geometry.py`; the SVG, PNG and
  DDS are deterministic outputs of `render_dds.py`.
- No bytes, colour samples or traced paths from the FusionFix asset were used.
  Only the fixed Controls-screen row coordinates (ten rows on the left, nine on
  the right) are shared, because they are the interface contract that keeps the
  surrounding text aligned.
- Licence: **GPL-3.0-or-later**, matching this repository.

DualSense and PlayStation are trademarks of Sony Interactive Entertainment
Inc. This is an independent illustration; it is not endorsed by or affiliated
with Sony. No Sony artwork is included.

## Regenerate

The generator has no third-party dependencies.

```sh
python3 tools/frontend_controller_patch/artwork/render_dds.py \
  --svg tools/frontend_controller_patch/artwork/dualsense_controller.svg \
  --png tools/frontend_controller_patch/artwork/dualsense_controller.png \
  --dds tools/frontend_controller_patch/artwork/dualsense_controller_512.dds \
  --ss 4 --strict
```

The hashes above are reproduced byte for byte.

## Use with the converter

The converter accepts any structurally valid 512×512, single-mip DXT5 input.
With custom artwork it validates and round-trips the dictionary, but it does not
match the byte-pinned vectors that were recorded with the FusionFix derivative.

```sh
# GTA IV (base, inspection only; the runtime patches the dictionary itself)
gta4_frontend_controller_patch convert --game gta4 \
  --input /path/to/user-owned/frontend.xtd \
  --dds tools/frontend_controller_patch/artwork/dualsense_controller_512.dds \
  --output /path/to/gta4_frontend_custom.xtd

# TLAD
gta4_frontend_controller_patch convert --game tlad \
  --input /path/to/user-owned/frontend.xtd \
  --dds tools/frontend_controller_patch/artwork/dualsense_controller_512.dds \
  --output /path/to/tlad_frontend_custom.xtd

# TBOGT
gta4_frontend_controller_patch convert --game tbogt \
  --input /path/to/user-owned/frontend.xtd \
  --dds tools/frontend_controller_patch/artwork/dualsense_controller_512.dds \
  --output /path/to/tbogt_frontend_custom.xtd
```

For the base game, copy the DDS to the packaged resources folder as
`button_prompts/ps5/dds_textures/controller_512.dds` (or
`button_prompts/<variant>/dds_textures/controller_512.dds` for another
controller variant). The runtime loader looks there first, then falls back to
the build-time asset root.

## Validation

Performed on 2026-11-14 with the stock dictionaries and the tool built from this
branch:

- DDS header: 512×512, DXT5, 0 mips, 262,144-byte payload (262,272 bytes with
  the 128-byte header).
- Generator round trip: RMSE 1.946, peak error 27/255.
- Leader-row detection: PASS for all ten left rows and nine right rows.
- `convert` result for each title (custom artwork, structurally validated, not
  byte-pinned):

| Game | Output size | Output SHA-256 |
| --- | ---: | --- |
| GTA IV | 1,430,448 | `ba7824d3365fcf8a0237ca1fd73867fdea9787322c29541a5fc40366fa484ad1` |
| TLAD | 1,430,448 | `7d044ebb2ff1847f86d6f3e8d3374fe72fe489c0fbdc77bf5fb9d4b9dec61d0e` |
| TBOGT | 1,446,853 | `d637781556e30a6bf7cd974b52c2b4e961d41bac5c1cf0adbf2d438bd68fd9b2` |

Control run with the non-distributed FusionFix derivative
(`f844b50a…`) reproduced the pinned vectors exactly:

| Game | Output SHA-256 |
| --- | --- |
| GTA IV | `3d6a1658fd80ed5307837445f00734c060efb51f3fdd5e2acf5b28aac0a0c620` |
| TLAD | `85ae8912d55f98be20ec0e213e9f4a6e09bc0c8f956d6187091b18add7c739b8` |
| TBOGT | `19006797766794618bdbacb2f657f39f1271294c62e46ddb76ea5fbae66b9b35` |

## Licensing

- The artwork and generator in this directory are GPL-3.0-or-later.
- The FusionFix-derived DDS, the retail dictionary, and the generated
  `frontend.xtd` outputs are not distributed here. Treat them as user-owned
  game data.
- The diagram is an independent illustration of a DualSense controller. Sony
  and PlayStation trademarks remain the property of their owners.

#!/usr/bin/env python3
"""
Convert PC GTA IV texture mods (.wtd, .img, .rpf, loose .dds, or FusionFix-style
update/ folders) into PS5-compatible <xxh64>.dds replacement packs.

Example usage (stacking Nexus Mods 105, 282, 357):
  python3 tools/convert_texture_mod.py \
      --xbox /path/to/xbox360_game_root \
      --mod /path/to/warcaco_105 \
      --mod /path/to/ash735_vehicles_282 \
      --mod /path/to/ash735_misc_357 \
      --max-size 2048 \
      --out ./textures
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_AES_KEY_PATH = REPO_ROOT / "LibertyRecompLib" / "aes_key.bin"

RPF2_MAGIC = 0x32465052  # "RPF2" little-endian
IMG3_MAGIC = 0xA94E2A52
RSC5_MAGIC = b"RSC\x05"
RSC5_MAGIC_BE = b"\x05CSR"
XMEM_LZX_MAGIC = 0x0FF512EF

# Xenos TextureFormat enum values (matches rex::graphics::xenos::TextureFormat)
XENOS_FMT_8 = 2
XENOS_FMT_8_8_8_8 = 6
XENOS_FMT_DXT1 = 18
XENOS_FMT_DXT2_3 = 19
XENOS_FMT_DXT4_5 = 20
XENOS_FMT_DXN = 49
XENOS_FMT_DXT5A = 59

SUPPORTED_XENOS_FORMATS: Dict[int, Tuple[int, int, str]] = {
    # fmt_id: (block_extent, bytes_per_block, vk/dds_name)
    XENOS_FMT_8: (1, 1, "R8"),
    XENOS_FMT_8_8_8_8: (1, 4, "RGBA8"),
    XENOS_FMT_DXT1: (4, 8, "BC1"),
    XENOS_FMT_DXT2_3: (4, 16, "BC2"),
    XENOS_FMT_DXT4_5: (4, 16, "BC3"),
    XENOS_FMT_DXN: (4, 16, "BC5"),
    XENOS_FMT_DXT5A: (4, 8, "BC4"),
}

BASE_FORMAT_ALIASES: Dict[int, int] = {
    # GetBaseFormat() mapping from xenos::TextureFormat
    32: XENOS_FMT_8_8_8_8,  # k_8_8_8_8_AS_16_16_16_16
    33: XENOS_FMT_DXT1,     # k_DXT1_AS_16_16_16_16
    34: XENOS_FMT_DXT2_3,   # k_DXT2_3_AS_16_16_16_16
    35: XENOS_FMT_DXT4_5,   # k_DXT4_5_AS_16_16_16_16
    56: XENOS_FMT_8_8_8_8,  # k_8_8_8_8_GAMMA_EDRAM
}

# Pure-Python XXH64 implementation (seed=0) matching thirdparty/xxHash bit-for-bit.
_XXH_P1 = 0x9E3779B185EBCA87
_XXH_P2 = 0xC2B2AE3D27D4EB4F
_XXH_P3 = 0x165667B19E3779F9
_XXH_P4 = 0x85EBCA77C2B2AE63
_XXH_P5 = 0x27D4EB2F165667C5
_MASK64 = 0xFFFFFFFFFFFFFFFF


def _rotl64(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & _MASK64


def _xxh64_round(acc: int, lane: int) -> int:
    acc = (acc + lane * _XXH_P2) & _MASK64
    acc = _rotl64(acc, 31)
    return (acc * _XXH_P1) & _MASK64


def _xxh64_merge_round(acc: int, val: int) -> int:
    val = _xxh64_round(0, val)
    acc ^= val
    return (acc * _XXH_P1 + _XXH_P4) & _MASK64


def xxh64_py(data: bytes, seed: int = 0) -> int:
    n = len(data)
    pos = 0
    if n >= 32:
        v1 = (seed + _XXH_P1 + _XXH_P2) & _MASK64
        v2 = (seed + _XXH_P2) & _MASK64
        v3 = seed & _MASK64
        v4 = (seed - _XXH_P1) & _MASK64
        limit = n - 32
        while pos <= limit:
            k1, k2, k3, k4 = struct.unpack_from("<QQQQ", data, pos)
            v1 = _xxh64_round(v1, k1)
            v2 = _xxh64_round(v2, k2)
            v3 = _xxh64_round(v3, k3)
            v4 = _xxh64_round(v4, k4)
            pos += 32
        h = (_rotl64(v1, 1) + _rotl64(v2, 7) + _rotl64(v3, 12) + _rotl64(v4, 18)) & _MASK64
        h = _xxh64_merge_round(h, v1)
        h = _xxh64_merge_round(h, v2)
        h = _xxh64_merge_round(h, v3)
        h = _xxh64_merge_round(h, v4)
    else:
        h = (seed + _XXH_P5) & _MASK64

    h = (h + n) & _MASK64
    while pos + 8 <= n:
        k1 = struct.unpack_from("<Q", data, pos)[0]
        h ^= _xxh64_round(0, k1)
        h = (_rotl64(h, 27) * _XXH_P1 + _XXH_P4) & _MASK64
        pos += 8
    while pos + 4 <= n:
        k1 = struct.unpack_from("<I", data, pos)[0]
        h ^= (k1 * _XXH_P1) & _MASK64
        h = (_rotl64(h, 23) * _XXH_P2 + _XXH_P3) & _MASK64
        pos += 4
    while pos < n:
        h ^= (data[pos] * _XXH_P5) & _MASK64
        h = (_rotl64(h, 11) * _XXH_P1) & _MASK64
        pos += 1

    h ^= h >> 33
    h = (h * _XXH_P2) & _MASK64
    h ^= h >> 29
    h = (h * _XXH_P3) & _MASK64
    h ^= h >> 32
    return h


_NATIVE_HELPER_C = r"""
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "mspack.h"
#include "lzx.h"
#include "aes.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

struct ChunkReader {
  const uint8_t* ptr;
  size_t len;
  size_t pos;
  int rem;
};

struct ChunkWriter {
  uint8_t* ptr;
  size_t len;
  size_t pos;
};

static int lzx_read_cb(struct mspack_file* f, void* dst, int bytes) {
  struct ChunkReader* r = (struct ChunkReader*)f;
  if (r->rem == 0) {
    if (r->pos + 2 > r->len) return 0;
    uint16_t chunk = ((uint16_t)r->ptr[r->pos] << 8) | r->ptr[r->pos + 1];
    r->pos += 2;
    if ((chunk & 0xFF00u) == 0xFF00u) {
      if (r->pos + 3 > r->len) return 0;
      r->pos += 1; /* skip low byte of uncompressed size; header is 5 bytes total */
      chunk = ((uint16_t)r->ptr[r->pos] << 8) | r->ptr[r->pos + 1];
      r->pos += 2;
    }
    if (chunk == 0) return 0;
    r->rem = (int)chunk;
  }
  size_t avail = (size_t)r->rem;
  if ((size_t)bytes < avail) avail = (size_t)bytes;
  if (r->len - r->pos < avail) avail = r->len - r->pos;
  memcpy(dst, r->ptr + r->pos, avail);
  r->pos += avail;
  r->rem -= (int)avail;
  return (int)avail;
}

static int lzx_write_cb(struct mspack_file* f, void* src, int bytes) {
  struct ChunkWriter* w = (struct ChunkWriter*)f;
  size_t avail = w->len - w->pos;
  if ((size_t)bytes < avail) avail = (size_t)bytes;
  memcpy(w->ptr + w->pos, src, avail);
  w->pos += avail;
  return (int)avail;
}

static void* lzx_alloc_cb(struct mspack_system* s, size_t n) { (void)s; return calloc(1, n); }
static void lzx_free_cb(void* p) { free(p); }
static void lzx_copy_cb(void* s, void* d, size_t n) { memcpy(d, s, n); }

API int gta4_xmem_lzx_decompress(const uint8_t* src, size_t src_len,
                                 uint8_t* dst, size_t dst_len,
                                 int window_bits) {
  struct mspack_system sys;
  memset(&sys, 0, sizeof(sys));
  sys.read = lzx_read_cb;
  sys.write = lzx_write_cb;
  sys.alloc = lzx_alloc_cb;
  sys.free = lzx_free_cb;
  sys.copy = lzx_copy_cb;

  struct ChunkReader reader = {src, src_len, 0, 0};
  struct ChunkWriter writer = {dst, dst_len, 0};
  struct lzxd_stream* s = lzxd_init(&sys, (struct mspack_file*)&reader,
                                    (struct mspack_file*)&writer,
                                    window_bits, 0, 0x8000, (off_t)dst_len, 0);
  if (!s) return -1;
  s->uncompressed_block_padding = 0;
  int rc = lzxd_decompress(s, (off_t)dst_len);
  lzxd_free(s);
  return (rc == MSPACK_ERR_OK && writer.pos == dst_len) ? 0 : -2;
}

API void gta4_aes256_ecb_decrypt_16rounds(const uint8_t* key32,
                                          uint8_t* buf, size_t buf_len) {
  struct AES_ctx ctx;
  AES_init_ctx(&ctx, key32);
  size_t aligned = buf_len & ~(size_t)15;
  for (int pass = 0; pass < 16; ++pass) {
    for (size_t off = 0; off < aligned; off += 16) {
      AES_ECB_decrypt(&ctx, buf + off);
    }
  }
}

API uint64_t gta4_canonical_hash(uint32_t width, uint32_t height,
                                 uint32_t base_format,
                                 const uint8_t* mip0, size_t mip0_len) {
  uint8_t meta[12];
  memcpy(meta + 0, &width, 4);
  memcpy(meta + 4, &height, 4);
  memcpy(meta + 8, &base_format, 4);
  XXH64_state_t* st = XXH64_createState();
  XXH64_reset(st, 0);
  XXH64_update(st, meta, 12);
  if (mip0 && mip0_len) XXH64_update(st, mip0, mip0_len);
  uint64_t h = XXH64_digest(st);
  XXH64_freeState(st);
  return h;
}

static inline uint32_t xenos_tiled_offset_2d(int32_t x, int32_t y,
                                             uint32_t pitch,
                                             uint32_t log2_bpp) {
  pitch = (pitch + 31u) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * (int32_t)(pitch >> 5)) << (log2_bpp + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return (uint32_t)(((offset & ~0x1FF) << 3) + ((y & 16) << 7) +
                    ((offset & 0x1C0) << 2) +
                    (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F));
}

API int gta4_extract_mip0_blocks(const uint8_t* src, size_t src_len,
                                 uint32_t blocks_w, uint32_t blocks_h,
                                 uint32_t guest_pitch_blocks,
                                 uint32_t bytes_per_block,
                                 int is_tiled, int endianness,
                                 uint8_t* dst) {
  if (!bytes_per_block || (bytes_per_block & (bytes_per_block - 1)) != 0) return -1;
  uint32_t log2_bpp = 0;
  for (uint32_t v = bytes_per_block; v > 1; v >>= 1) ++log2_bpp;

  for (uint32_t by = 0; by < blocks_h; ++by) {
    for (uint32_t bx = 0; bx < blocks_w; ++bx) {
      size_t src_off = is_tiled
          ? (size_t)xenos_tiled_offset_2d((int32_t)bx, (int32_t)by,
                                          guest_pitch_blocks, log2_bpp)
          : ((size_t)by * guest_pitch_blocks + bx) * bytes_per_block;
      if (src_off + bytes_per_block > src_len) return -2;
      const uint8_t* sp = src + src_off;
      uint8_t* dp = dst + ((size_t)by * blocks_w + bx) * bytes_per_block;
      if (bytes_per_block == 1 || endianness == 0) {
        memcpy(dp, sp, bytes_per_block);
      } else if (endianness == 1) { /* k8in16 */
        for (uint32_t k = 0; k < bytes_per_block; k += 2) {
          dp[k] = sp[k + 1];
          dp[k + 1] = sp[k];
        }
      } else if (endianness == 2) { /* k8in32 */
        for (uint32_t k = 0; k < bytes_per_block; k += 4) {
          dp[k] = sp[k + 3];
          dp[k + 1] = sp[k + 2];
          dp[k + 2] = sp[k + 1];
          dp[k + 3] = sp[k];
        }
      } else { /* k16in32 */
        for (uint32_t k = 0; k < bytes_per_block; k += 4) {
          dp[k] = sp[k + 2];
          dp[k + 1] = sp[k + 3];
          dp[k + 2] = sp[k];
          dp[k + 3] = sp[k + 1];
        }
      }
    }
  }
  return 0;
}
"""


class NativeHelper:
    """Lazy-compiled native helper reusing thirdparty libmspack/tiny-aes-c/xxHash."""

    def __init__(self) -> None:
        self._lib: Optional[ctypes.CDLL] = None
        self._attempted = False

    def _ensure_loaded(self) -> ctypes.CDLL:
        if self._lib is not None:
            return self._lib
        if self._attempted:
            raise RuntimeError("Native helper unavailable")
        self._attempted = True

        thirdparty = REPO_ROOT / "glue" / "rexglue-sdk-main" / "thirdparty"
        lzxd_c = thirdparty / "libmspack" / "libmspack" / "mspack" / "lzxd.c"
        aes_c = thirdparty / "tiny-aes-c" / "aes.c"
        xxh_dir = thirdparty / "xxHash"
        if not lzxd_c.exists() or not aes_c.exists() or not (xxh_dir / "xxhash.h").exists():
            raise FileNotFoundError(
                f"Expected thirdparty sources under {thirdparty} (run inside gta4-ps5 repo)"
            )

        digest = hashlib.sha256((_NATIVE_HELPER_C + "|AES256=1").encode("utf-8")).hexdigest()[:16]
        ext = ".dll" if os.name == "nt" else ".so"
        cache_dir = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "gta4-ps5"
        cache_dir.mkdir(parents=True, exist_ok=True)
        out_lib = cache_dir / f"gta4_mod_helper_{digest}{ext}"

        if not out_lib.exists():
            compiler = (
                shutil.which("gcc")
                or shutil.which("clang")
                or shutil.which("cc")
            )
            if not compiler:
                raise RuntimeError(
                    "No C compiler (gcc/clang/cc) found in PATH to build libmspack/AES helper."
                )
            with tempfile.TemporaryDirectory() as tmp:
                c_src = Path(tmp) / "helper.c"
                c_src.write_text(_NATIVE_HELPER_C, encoding="utf-8")
                tmp_out = Path(tmp) / f"helper{ext}"
                cmd = [
                    compiler,
                    "-O2",
                    "-DAES256=1",
                    "-shared",
                    *([] if os.name == "nt" else ["-fPIC"]),
                    "-I", str(lzxd_c.parent),
                    "-I", str(aes_c.parent),
                    "-I", str(xxh_dir),
                    str(c_src),
                    str(lzxd_c),
                    str(aes_c),
                    "-o", str(tmp_out),
                ]
                proc = subprocess.run(cmd, capture_output=True, text=True)
                if proc.returncode != 0:
                    raise RuntimeError(f"Failed compiling helper:\n{proc.stderr}")
                shutil.move(str(tmp_out), str(out_lib))

        lib = ctypes.CDLL(str(out_lib))
        lib.gta4_xmem_lzx_decompress.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_int,
        ]
        lib.gta4_xmem_lzx_decompress.restype = ctypes.c_int

        lib.gta4_aes256_ecb_decrypt_16rounds.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ]
        lib.gta4_aes256_ecb_decrypt_16rounds.restype = None

        lib.gta4_canonical_hash.argtypes = [
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_void_p, ctypes.c_size_t,
        ]
        lib.gta4_canonical_hash.restype = ctypes.c_uint64

        lib.gta4_extract_mip0_blocks.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t,
            ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
            ctypes.c_int, ctypes.c_int,
            ctypes.c_void_p,
        ]
        lib.gta4_extract_mip0_blocks.restype = ctypes.c_int

        self._lib = lib
        return lib

    def aes_decrypt_16rounds(self, key32: bytes, data: bytes) -> bytes:
        lib = self._ensure_loaded()
        out = bytearray(data)
        k_buf = (ctypes.c_uint8 * 32).from_buffer_copy(key32)
        d_buf = (ctypes.c_uint8 * len(out)).from_buffer(out)
        lib.gta4_aes256_ecb_decrypt_16rounds(k_buf, d_buf, len(out))
        return bytes(out)

    def xmem_lzx_decompress(self, src: bytes, expected_uncompressed: int) -> bytes:
        lib = self._ensure_loaded()
        # Sum the XMem LZX chunk sizes as an upper bound so padded resources also decode cleanly.
        total_chunks = 0
        p = 0
        n = len(src)
        while p + 2 <= n:
            if src[p] == 0xFF and p + 5 <= n:
                u, c = struct.unpack_from(">HH", src, p + 1)
                p += 5
            else:
                u = 0x8000
                c = struct.unpack_from(">H", src, p)[0]
                p += 2
            if c == 0:
                break
            total_chunks += u
            p += c

        target_len = max(expected_uncompressed, total_chunks)
        s_buf = (ctypes.c_uint8 * len(src)).from_buffer_copy(src)
        for window_bits in (17, 16, 15):
            dst = bytearray(target_len)
            d_buf = (ctypes.c_uint8 * target_len).from_buffer(dst)
            rc = lib.gta4_xmem_lzx_decompress(s_buf, len(src), d_buf, target_len, window_bits)
            if rc == 0:
                return bytes(dst[:expected_uncompressed])
        raise ValueError("XMem LZX decompression failed")

    def extract_mip0_blocks(
        self,
        src: bytes,
        blocks_w: int,
        blocks_h: int,
        guest_pitch_blocks: int,
        bytes_per_block: int,
        is_tiled: bool,
        endianness: int,
    ) -> bytes:
        lib = self._ensure_loaded()
        out_len = blocks_w * blocks_h * bytes_per_block
        dst = bytearray(out_len)
        s_buf = (ctypes.c_uint8 * len(src)).from_buffer_copy(src)
        d_buf = (ctypes.c_uint8 * out_len).from_buffer(dst)
        rc = lib.gta4_extract_mip0_blocks(
            s_buf, len(src),
            blocks_w, blocks_h, guest_pitch_blocks, bytes_per_block,
            1 if is_tiled else 0, endianness, d_buf,
        )
        if rc != 0:
            raise ValueError(f"extract_mip0_blocks failed (rc={rc})")
        return bytes(dst)

    def canonical_hash(self, width: int, height: int, base_format: int, mip0: bytes) -> int:
        try:
            lib = self._ensure_loaded()
            m_buf = (ctypes.c_uint8 * len(mip0)).from_buffer_copy(mip0)
            return int(lib.gta4_canonical_hash(width, height, base_format, m_buf, len(mip0)))
        except Exception:
            meta = struct.pack("<III", width, height, base_format)
            return xxh64_py(meta + mip0, 0)


HELPER = NativeHelper()


def normalize_tex_name(raw: str) -> str:
    s = raw.strip().replace("\\", "/")
    if s.startswith("pack:/"):
        s = s[6:]
    if "/" in s:
        s = s.rsplit("/", 1)[-1]
    lower = s.lower()
    for ext in (".dds", ".png", ".tga"):
        if lower.endswith(ext):
            s = s[: -len(ext)]
            break
    return s.lower()


def _rsc_ptr_to_offset(ptr: int, vsize: int, total_size: int) -> Optional[int]:
    if ptr == 0:
        return None
    sec = ptr >> 28
    rel = ptr & 0x0FFFFFFF
    if sec == 5:
        off = rel
    elif sec == 6:
        off = vsize + rel
    else:
        return None
    return off if off < total_size else None


def _rsc_segment_sizes(flags: int) -> Tuple[int, int]:
    vsize = (flags & 0x7FF) << (((flags >> 11) & 0xF) + 8)
    psize = ((flags >> 15) & 0x7FF) << (((flags >> 26) & 0xF) + 8)
    return vsize, psize


@dataclass
class XboxTextureEntry:
    hash_hex: str
    dict_name: str
    tex_name: str
    width: int
    height: int
    base_format: int
    mip0_linear: Optional[bytes] = None


def parse_xtd_bytes(
    data: bytes,
    dict_name: str,
    keep_mip0: bool = False,
) -> List[XboxTextureEntry]:
    if len(data) < 20:
        return []
    magic = data[:4]
    if magic == RSC5_MAGIC_BE:
        _res_type, flags = struct.unpack_from(">II", data, 4)
    elif magic == RSC5_MAGIC:
        _res_type, flags = struct.unpack_from("<II", data, 4)
    else:
        return []
    vsize, psize = _rsc_segment_sizes(flags)
    total = vsize + psize
    if total == 0 or total > 256 * 1024 * 1024:
        return []

    magic2 = struct.unpack_from(">I", data, 12)[0]
    if magic2 == XMEM_LZX_MAGIC:
        csize = struct.unpack_from(">I", data, 16)[0]
        stream = data[20 : 20 + csize] if 20 + csize <= len(data) else data[20:]
        raw = HELPER.xmem_lzx_decompress(stream, total)
    elif len(data) >= 14 and data[12] == 0x78:
        raw = zlib.decompress(data[12:])[:total]
    elif len(data) >= 18 and data[16] == 0x78:
        raw = zlib.decompress(data[16:])[:total]
    else:
        raw = data[12:]

    if len(raw) < 28:
        return []

    cnt, _cnt2, list_ptr = struct.unpack_from(">HHI", raw, 20)
    list_off = _rsc_ptr_to_offset(list_ptr, vsize, len(raw))
    if list_off is None or list_off + cnt * 4 > len(raw):
        return []

    entries: List[XboxTextureEntry] = []
    for i in range(cnt):
        tex_ptr = struct.unpack_from(">I", raw, list_off + i * 4)[0]
        to = _rsc_ptr_to_offset(tex_ptr, vsize, len(raw))
        if to is None or to + 32 > len(raw):
            continue
        name_ptr, d3d_ptr, w_field, h_field = struct.unpack_from(">IIHH", raw, to + 20)
        no = _rsc_ptr_to_offset(name_ptr, vsize, len(raw))
        do = _rsc_ptr_to_offset(d3d_ptr, vsize, len(raw))
        if no is None or do is None or do + 52 > len(raw):
            continue
        nul = raw.find(b"\x00", no)
        raw_name = raw[no : nul if nul != -1 else len(raw)].decode("ascii", errors="replace")
        tex_name = normalize_tex_name(raw_name)

        dwords = struct.unpack_from(">13I", raw, do)
        dw0, dw1, dw2, _dw3, _dw4, dw5 = dwords[7:13]
        fmt_raw = dw1 & 0x3F
        base_format = BASE_FORMAT_ALIASES.get(fmt_raw, fmt_raw)
        if base_format not in SUPPORTED_XENOS_FORMATS:
            continue
        if ((dw5 >> 9) & 0x3) not in (0, 1):  # 1D or 2D
            continue
        if (dw1 >> 10) & 0x1:  # stacked
            continue
        if (_dw4 >> 2) & 0xF != 0:  # mip_min_level != 0
            continue

        width = (dw2 & 0x1FFF) + 1 if dw2 else w_field
        height = ((dw2 >> 13) & 0x1FFF) + 1 if dw2 else h_field
        tiled = bool((dw0 >> 31) & 1)
        pitch_pixels = ((dw0 >> 22) & 0x1FF) << 5
        endianness = (dw1 >> 6) & 0x3
        phys_ptr = (dw1 >> 12) << 12
        phys_off = _rsc_ptr_to_offset(phys_ptr, vsize, len(raw))
        if phys_off is None:
            continue

        block_ext, bpb, _ = SUPPORTED_XENOS_FORMATS[base_format]
        blocks_w = max(1, (width + block_ext - 1) // block_ext)
        blocks_h = max(1, (height + block_ext - 1) // block_ext)
        # Match TextureInfo::GetMipExtent(..., is_guest=true): pitch aligned to tile if tiled, blocks_per_row
        pitch_blocks = max(1, pitch_pixels // block_ext) if pitch_pixels else blocks_w
        if tiled:
            pitch_blocks = (pitch_blocks + 31) & ~31

        try:
            mip0 = HELPER.extract_mip0_blocks(
                raw[phys_off:], blocks_w, blocks_h, pitch_blocks, bpb, tiled, endianness
            )
        except Exception:
            continue

        h = HELPER.canonical_hash(width, height, base_format, mip0)
        entries.append(
            XboxTextureEntry(
                hash_hex=f"{h:016x}",
                dict_name=dict_name,
                tex_name=tex_name,
                width=width,
                height=height,
                base_format=base_format,
                mip0_linear=mip0 if keep_mip0 else None,
            )
        )
    return entries


@dataclass
class PcTextureAsset:
    dict_name: Optional[str]
    tex_name: str
    dds_bytes: bytes
    source_desc: str


def build_dds_bytes(
    width: int,
    height: int,
    mip_count: int,
    mip_payload: bytes,
    fourcc: Optional[bytes] = None,
    dxgi_format: Optional[int] = None,
    rgb_bpp: int = 32,
    masks: Tuple[int, int, int, int] = (0, 0, 0, 0),
    is_compressed: bool = True,
) -> bytes:
    flags = 0x1 | 0x2 | 0x4 | 0x1000  # CAPS | HEIGHT | WIDTH | PIXELFORMAT
    if mip_count > 1:
        flags |= 0x20000  # MIPMAPCOUNT
    caps = 0x1000
    if mip_count > 1:
        caps |= 0x400008

    if is_compressed:
        flags |= 0x80000  # LINEARSIZE
        bpb = 8 if (fourcc == b"DXT1" or fourcc == b"ATI1" or dxgi_format in (71, 80)) else 16
        pitch_or_linear = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * bpb
    else:
        flags |= 0x8  # PITCH
        pitch_or_linear = width * (rgb_bpp // 8)

    if dxgi_format is not None:
        ddspf = struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
        dx10 = struct.pack("<IIIII", dxgi_format, 3, 0, 1, 0)
    elif fourcc is not None:
        ddspf = struct.pack("<II4sIIIII", 32, 0x4, fourcc, 0, 0, 0, 0, 0)
        dx10 = b""
    else:
        r_mask, g_mask, b_mask, a_mask = masks
        pf_flags = 0x20000 if (rgb_bpp == 8 and g_mask == 0) else (0x40 | (0x1 if a_mask else 0))
        ddspf = struct.pack("<II4sIIIII", 32, pf_flags, b"\x00\x00\x00\x00", rgb_bpp, r_mask, g_mask, b_mask, a_mask)
        dx10 = b""

    header = struct.pack(
        "<IIIIIII44s",
        124,
        flags,
        height,
        width,
        pitch_or_linear,
        0,
        mip_count,
        b"\x00" * 44,
    )
    tail = struct.pack("<IIIII", caps, 0, 0, 0, 0)
    return b"DDS " + header + ddspf + tail + dx10 + mip_payload


def parse_wtd_bytes(data: bytes, dict_name: str, source_desc: str) -> List[PcTextureAsset]:
    if len(data) < 20 or data[:4] != RSC5_MAGIC:
        return []
    _res_type, flags = struct.unpack_from("<II", data, 4)
    vsize, psize = _rsc_segment_sizes(flags)
    total = vsize + psize
    if total == 0 or total > 256 * 1024 * 1024:
        return []

    def _inflate_zlib(buf: bytes) -> bytes:
        try:
            return zlib.decompress(buf)
        except zlib.error:
            # Some modded WTDs repacked by SparkIV/OpenIV omit or miscompute the trailing Adler32.
            return zlib.decompressobj(-15).decompress(buf[2:])

    if data[12] == 0x78:
        raw = _inflate_zlib(data[12:])
    elif len(data) > 18 and data[16] == 0x78:
        raw = _inflate_zlib(data[16:])
    else:
        raw = data[12:]

    if len(raw) < 28:
        return []

    cnt, _cnt2, list_ptr = struct.unpack_from("<HHI", raw, 20)
    list_off = _rsc_ptr_to_offset(list_ptr, vsize, len(raw))
    if list_off is None or list_off + cnt * 4 > len(raw):
        return []

    assets: List[PcTextureAsset] = []
    for i in range(cnt):
        tex_ptr = struct.unpack_from("<I", raw, list_off + i * 4)[0]
        to = _rsc_ptr_to_offset(tex_ptr, vsize, len(raw))
        if to is None or to + 64 > len(raw):
            continue

        name_ptr = struct.unpack_from("<I", raw, to + 0x14)[0]
        no = _rsc_ptr_to_offset(name_ptr, vsize, len(raw))
        if no is None:
            continue
        nul = raw.find(b"\x00", no)
        raw_name = raw[no : nul if nul != -1 else len(raw)].decode("ascii", errors="replace")
        tex_name = normalize_tex_name(raw_name)

        width, height, fmt_u32, _stride, tex_type, mip_levels = struct.unpack_from("<HHIHBB", raw, to + 0x1C)
        if not width or not height or tex_type != 0:
            continue
        mip_levels = max(1, mip_levels)

        data_off = None
        for ptr_cand in (to + 0x48, to + 0x44, to + 0x3C, to + 0x40):
            if ptr_cand + 4 <= len(raw):
                cand = struct.unpack_from("<I", raw, ptr_cand)[0]
                if (cand >> 28) == 6:
                    data_off = _rsc_ptr_to_offset(cand, vsize, len(raw))
                    if data_off is not None:
                        break
        if data_off is None:
            continue

        fmt_cc = struct.pack("<I", fmt_u32)
        fourcc = None
        dxgi_format = None
        is_comp = True
        bpb = 16
        blk_ext = 4
        rgb_bpp = 32
        masks = (0, 0, 0, 0)

        if fmt_cc in (b"DXT1", b"DXT3", b"DXT5"):
            fourcc = fmt_cc
            bpb = 8 if fmt_cc == b"DXT1" else 16
        elif fmt_cc in (b"ATI1", b"BC4U"):
            dxgi_format = 80
            bpb = 8
        elif fmt_cc in (b"ATI2", b"BC5U"):
            dxgi_format = 83
            bpb = 16
        elif fmt_u32 in (21, 22):  # D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8
            is_comp = False
            blk_ext = 1
            bpb = 4
            rgb_bpp = 32
            masks = (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000 if fmt_u32 == 21 else 0)
        elif fmt_u32 in (28, 50):  # D3DFMT_A8, D3DFMT_L8
            is_comp = False
            blk_ext = 1
            bpb = 1
            rgb_bpp = 8
            masks = (0xFF, 0, 0, 0)
        else:
            continue

        total_bytes = 0
        for lvl in range(mip_levels):
            mw = max(1, width >> lvl)
            mh = max(1, height >> lvl)
            bw = max(1, (mw + blk_ext - 1) // blk_ext)
            bh = max(1, (mh + blk_ext - 1) // blk_ext)
            total_bytes += bw * bh * bpb

        if data_off + total_bytes > len(raw):
            continue

        dds_bytes = build_dds_bytes(
            width, height, mip_levels, raw[data_off : data_off + total_bytes],
            fourcc=fourcc, dxgi_format=dxgi_format, rgb_bpp=rgb_bpp, masks=masks, is_compressed=is_comp,
        )
        assets.append(
            PcTextureAsset(
                dict_name=dict_name,
                tex_name=tex_name,
                dds_bytes=dds_bytes,
                source_desc=f"{source_desc}:{tex_name}",
            )
        )
    return assets


def downcap_dds_by_mips(dds_bytes: bytes, max_size: int) -> bytes:
    """Drop largest mip levels natively (zero recompression) if max_size > 0 and mips exist."""
    if max_size <= 0 or len(dds_bytes) < 128 or dds_bytes[:4] != b"DDS ":
        return dds_bytes
    h, w, _lin, _d, mip_cnt = struct.unpack_from("<IIIII", dds_bytes, 12)
    if mip_cnt <= 1 or (w <= max_size and h <= max_size):
        return dds_bytes

    pf_flags, fourcc = struct.unpack_from("<I4s", dds_bytes, 80)
    rgb_bits, r_mask, g_mask, b_mask, a_mask = struct.unpack_from("<IIIII", dds_bytes, 88)
    data_off = 128
    dxgi_fmt = None
    if (pf_flags & 0x4) and fourcc == b"DX10":
        if len(dds_bytes) < 148:
            return dds_bytes
        dxgi_fmt = struct.unpack_from("<I", dds_bytes, 128)[0]
        data_off = 148

    blk_ext = 1
    bpb = 0
    if dxgi_fmt is not None:
        if dxgi_fmt in (71, 72, 80):
            blk_ext, bpb = 4, 8
        elif dxgi_fmt in (74, 75, 77, 78, 83, 98, 99):
            blk_ext, bpb = 4, 16
        elif dxgi_fmt in (28, 29, 87, 88, 91):
            blk_ext, bpb = 1, 4
        elif dxgi_fmt == 49:
            blk_ext, bpb = 1, 2
        elif dxgi_fmt == 61:
            blk_ext, bpb = 1, 1
    elif pf_flags & 0x4:
        if fourcc in (b"DXT1", b"ATI1", b"BC4U"):
            blk_ext, bpb = 4, 8
        elif fourcc in (b"DXT3", b"DXT5", b"ATI2", b"BC5U"):
            blk_ext, bpb = 4, 16
    elif rgb_bits in (8, 24, 32):
        blk_ext, bpb = 1, rgb_bits // 8

    if bpb == 0:
        return dds_bytes

    cursor = data_off
    skip_until_lvl = 0
    cur_w, cur_h = w, h
    while skip_until_lvl < mip_cnt - 1 and (cur_w > max_size or cur_h > max_size):
        bw = max(1, (cur_w + blk_ext - 1) // blk_ext)
        bh = max(1, (cur_h + blk_ext - 1) // blk_ext)
        mip_sz = bw * bh * bpb
        if cursor + mip_sz >= len(dds_bytes):
            return dds_bytes
        cursor += mip_sz
        skip_until_lvl += 1
        cur_w = max(1, cur_w >> 1)
        cur_h = max(1, cur_h >> 1)

    if skip_until_lvl == 0:
        return dds_bytes

    new_mip_cnt = mip_cnt - skip_until_lvl
    buf = bytearray(dds_bytes[:data_off] + dds_bytes[cursor:])
    struct.pack_into("<II", buf, 12, cur_h, cur_w)
    bw0 = max(1, (cur_w + blk_ext - 1) // blk_ext)
    bh0 = max(1, (cur_h + blk_ext - 1) // blk_ext)
    lin0 = bw0 * bh0 * bpb if blk_ext > 1 else cur_w * bpb
    struct.pack_into("<I", buf, 20, lin0)
    struct.pack_into("<I", buf, 28, new_mip_cnt)
    return bytes(buf)


WANTED_ARCHIVE_EXTS = (".rpf", ".img")
WANTED_XBOX_EXTS = (".rpf", ".img", ".xtd")
WANTED_MOD_EXTS = (".rpf", ".img", ".wtd", ".dds")


def _is_skipped_xbox_path(path_low: str) -> bool:
    return (
        "/audio/" in path_low
        or path_low.startswith("audio/")
        or "/anim/" in path_low
        or path_low.startswith("anim/")
        or path_low.endswith(("/audio.rpf", "audio.rpf", "navmeshes.img", "paths.img", "carrec.img", "script.img", "script_network.img", "navgen_script.img"))
    )


def iter_rpf2_files(
    raw_bytes: bytes,
    aes_key: Optional[bytes],
    archive_name: str,
    wanted_exts: Tuple[str, ...] = (".rpf", ".img", ".xtd", ".wtd", ".dds"),
) -> Iterable[Tuple[str, bytes]]:
    if len(raw_bytes) < 2048 or struct.unpack_from("<I", raw_bytes, 0)[0] != RPF2_MAGIC:
        return
    _magic, toc_size, entry_count, _resv, encrypted = struct.unpack_from("<IIIII", raw_bytes, 0)
    if entry_count == 0 or toc_size < entry_count * 16 or 2048 + toc_size > len(raw_bytes):
        return
    toc = raw_bytes[2048 : 2048 + toc_size]
    if encrypted != 0:
        if not aes_key:
            raise RuntimeError(f"Encrypted RPF2 '{archive_name}' requires --aes-key")
        toc = HELPER.aes_decrypt_16rounds(aes_key, toc)

    str_base = entry_count * 16
    entries = [struct.unpack_from("<IIII", toc, i * 16) for i in range(entry_count)]

    def entry_name(name_off: int) -> str:
        s = str_base + name_off
        e = toc.find(b"\x00", s)
        return toc[s : e if e != -1 else len(toc)].decode("ascii", errors="replace")

    stack = [(0, "")]
    visited = set()
    while stack:
        idx, prefix = stack.pop()
        if idx in visited or idx >= entry_count:
            continue
        visited.add(idx)
        name_off, w1, w2, w3 = entries[idx]
        name = entry_name(name_off) if idx != 0 else ""
        rel_path = f"{prefix}/{name}".lstrip("/") if name else prefix
        is_dir = bool(w2 & 0x80000000)
        if is_dir:
            start = w2 & 0x7FFFFFFF
            count = w3 & 0x0FFFFFFF
            for child in range(start, min(entry_count, start + count)):
                stack.append((child, rel_path))
        else:
            if not rel_path.lower().endswith(wanted_exts):
                continue
            is_resource = (w3 & 0xC0000000) == 0xC0000000
            if is_resource:
                off = w2 & 0x7FFFFF00
                arch_sz = w1
                chunk = raw_bytes[off : off + arch_sz]
            else:
                off = w2
                arch_sz = w3 & 0x3FFFFFFF
                is_comp = bool(w3 & 0x40000000)
                chunk = raw_bytes[off : off + arch_sz]
                if is_comp:
                    chunk = zlib.decompress(chunk, -15)
            yield rel_path, chunk


def iter_img3_files(
    raw_bytes: bytes,
    aes_key: Optional[bytes],
    archive_name: str,
    wanted_exts: Tuple[str, ...] = (".rpf", ".img", ".xtd", ".wtd", ".dds"),
) -> Iterable[Tuple[str, bytes]]:
    if len(raw_bytes) < 20:
        return
    hdr = raw_bytes[:20]
    magic, version, num_items, toc_size, _ = struct.unpack_from("<IIIII", hdr, 0)
    encrypted = False
    if magic != IMG3_MAGIC or version != 3:
        if not aes_key:
            return
        dec_hdr = HELPER.aes_decrypt_16rounds(aes_key, hdr[:16])
        magic, version, num_items, toc_size = struct.unpack_from("<IIII", dec_hdr, 0)
        if magic != IMG3_MAGIC or version != 3:
            return
        encrypted = True

    if toc_size < num_items * 16 or 20 + toc_size > len(raw_bytes):
        return
    toc = raw_bytes[20 : 20 + toc_size]
    if encrypted and aes_key:
        toc = HELPER.aes_decrypt_16rounds(aes_key, toc)

    str_pos = num_items * 16
    for i in range(num_items):
        sz_or_flags, _res_type, blk_off, used_blks, flags = struct.unpack_from("<IIIHH", toc, i * 16)
        nul = toc.find(b"\x00", str_pos)
        if nul == -1:
            break
        name = toc[str_pos:nul].decode("ascii", errors="replace")
        str_pos = nul + 1

        if not name.lower().endswith(wanted_exts):
            continue
        off = blk_off * 2048
        alloc = used_blks * 2048
        pad = flags & 0x7FF
        exact = alloc - pad if alloc >= pad and alloc > 0 else sz_or_flags
        if off + exact <= len(raw_bytes):
            yield name, raw_bytes[off : off + exact]


def iter_gdfx_iso_files(iso_path: Path, wanted_exts: Tuple[str, ...]) -> Iterable[Tuple[str, bytes]]:
    """Read matching files directly from an Xbox 360 GDFX ISO."""
    sector_size = 2048
    likely_offsets = (0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000)
    with iso_path.open("rb") as fp:
        fp.seek(0, os.SEEK_END)
        iso_size = fp.tell()
        game_offset = None
        for cand in likely_offsets:
            desc_off = cand + 32 * sector_size
            if desc_off + 28 <= iso_size:
                fp.seek(desc_off)
                desc = fp.read(28)
                if desc[:20] == b"MICROSOFT*XBOX*MEDIA":
                    game_offset = cand
                    root_sector, root_size = struct.unpack_from("<II", desc, 20)
                    break
        if game_offset is None or root_size < 13:
            return

        def read_at(off: int, length: int) -> bytes:
            fp.seek(off)
            return fp.read(length)

        def walk_dir(dir_buf: bytes, prefix: str) -> Iterable[Tuple[str, bytes]]:
            stack = [0]
            visited = set()
            while stack:
                ord_idx = stack.pop()
                if ord_idx in visited:
                    continue
                visited.add(ord_idx)
                off = ord_idx * 4
                if off + 14 > len(dir_buf):
                    continue
                node_l, node_r, sector, length, attrs, name_len = struct.unpack_from("<HHIIBB", dir_buf, off)
                if name_len == 0 or off + 14 + name_len > len(dir_buf):
                    continue
                name = dir_buf[off + 14 : off + 14 + name_len].decode("ascii", errors="replace")
                rel = f"{prefix}/{name}".lstrip("/")
                if node_r:
                    stack.append(node_r)
                if node_l:
                    stack.append(node_l)
                rel_low = rel.lower()
                if attrs & 0x10:
                    if length > 0 and not _is_skipped_xbox_path(rel_low + "/"):
                        sub_buf = read_at(game_offset + sector * sector_size, length)
                        yield from walk_dir(sub_buf, rel)
                elif rel_low.endswith(wanted_exts) and not _is_skipped_xbox_path(rel_low):
                    data = read_at(game_offset + sector * sector_size, length)
                    yield rel, data

        root_buf = read_at(game_offset + root_sector * sector_size, root_size)
        yield from walk_dir(root_buf, "")


def iter_stfs_files(stfs_path: Path, wanted_exts: Tuple[str, ...]) -> Iterable[Tuple[str, bytes]]:
    """Read matching files from an Xbox 360 STFS (LIVE/PIRS/CON) DLC/update container."""
    with stfs_path.open("rb") as fp:
        hdr = fp.read(0x400)
        if len(hdr) < 0x3B0 or hdr[:4] not in (b"LIVE", b"PIRS", b"CON "):
            return
        header_size = struct.unpack_from(">I", hdr, 0x340)[0]
        vol_type = struct.unpack_from(">I", hdr, 0x3A9)[0]
        if vol_type != 0:
            return
        desc = hdr[0x379 : 0x379 + 0x24]
        if desc[0] != 0x24:
            return
        vol_flags = desc[2]
        read_only = bool(vol_flags & 1)
        root_active = bool(vol_flags & 2)
        ft_block_cnt = struct.unpack_from("<H", desc, 3)[0]
        ft_start_blk = desc[5] | (desc[6] << 8) | (desc[7] << 16)
        total_blocks = struct.unpack_from(">I", desc, 0x1C)[0]

        bsize = 0x1000
        bph = 1 if read_only else 2
        step0 = 170 + bph
        step1 = 28900 + (170 + 1) * bph
        data_base = ((header_size + bsize - 1) // bsize) * bsize

        def blk_to_data_off(blk_idx: int) -> int:
            b = blk_idx
            base = 170
            for _ in range(3):
                b += ((blk_idx + base) // base) * bph
                if blk_idx < base:
                    break
                base *= 170
            return data_base + (b << 12)

        def hash_blk_off(blk_idx: int, lvl: int) -> int:
            if lvl == 0:
                if blk_idx < 170:
                    hb = 0
                else:
                    hb = (blk_idx // 170) * step0 + ((blk_idx // 28900) + 1) * bph
                    if blk_idx >= 28900:
                        hb += bph
            elif lvl == 1:
                if blk_idx < 28900:
                    hb = step0
                else:
                    hb = (blk_idx // 28900) * step1 + bph
            else:
                hb = step1
            return data_base + (hb << 12)

        hash_cache: Dict[int, bytes] = {}

        def read_cached(off: int) -> bytes:
            if off not in hash_cache:
                fp.seek(off)
                hash_cache[off] = fp.read(bsize)
            return hash_cache[off]

        def next_block(blk_idx: int) -> int:
            sec = 0 if read_only else (bsize if root_active else 0)
            if not read_only and total_blocks > 170:
                if total_blocks > 28900:
                    t2 = read_cached(hash_blk_off(blk_idx, 2) + sec)
                    r2 = (blk_idx // 28900) % 170
                    info2 = struct.unpack_from(">I", t2, r2 * 24 + 20)[0]
                    sec = bsize if (info2 & 0x40000000) else 0
                t1 = read_cached(hash_blk_off(blk_idx, 1) + sec)
                r1 = (blk_idx // 170) % 170
                info1 = struct.unpack_from(">I", t1, r1 * 24 + 20)[0]
                sec = bsize if (info1 & 0x40000000) else 0
            t0 = read_cached(hash_blk_off(blk_idx, 0) + sec)
            r0 = blk_idx % 170
            info0 = struct.unpack_from(">I", t0, r0 * 24 + 20)[0]
            return info0 & 0xFFFFFF

        entries_meta: List[Tuple[str, bool, bool, int, int, int]] = []
        cur_blk = ft_start_blk
        for _ in range(ft_block_cnt):
            if cur_blk == 0xFFFFFF:
                break
            fp.seek(blk_to_data_off(cur_blk))
            blk_data = fp.read(bsize)
            done = False
            for m in range(64):
                ent = blk_data[m * 64 : (m + 1) * 64]
                if ent[0] == 0:
                    done = True
                    break
                flags = ent[40]
                nlen = flags & 0x3F
                contig = bool((flags >> 6) & 1)
                is_dir = bool((flags >> 7) & 1)
                name = ent[:nlen].decode("ascii", errors="replace")
                start_blk = ent[47] | (ent[48] << 8) | (ent[49] << 16)
                parent_idx, length = struct.unpack_from(">HI", ent, 50)
                entries_meta.append((name, is_dir, contig, start_blk, parent_idx, length))
            if done:
                break
            cur_blk = next_block(cur_blk)

        def build_path(idx: int) -> str:
            parts = []
            cur = idx
            seen = set()
            while cur != 0xFFFF and cur < len(entries_meta) and cur not in seen:
                seen.add(cur)
                parts.append(entries_meta[cur][0])
                cur = entries_meta[cur][4]
            return "/".join(reversed(parts))

        for idx, (name, is_dir, contig, start_blk, _parent_idx, length) in enumerate(entries_meta):
            if is_dir or length == 0:
                continue
            rel = build_path(idx)
            rel_low = rel.lower()
            if not rel_low.endswith(wanted_exts) or _is_skipped_xbox_path(rel_low):
                continue
            out = bytearray()
            rem = length
            b = start_blk
            while rem > 0 and b != 0xFFFFFF:
                take = min(bsize, rem)
                fp.seek(blk_to_data_off(b))
                out.extend(fp.read(take))
                rem -= take
                if rem > 0:
                    b = (b + 1) if contig else next_block(b)
            yield rel, bytes(out)


def walk_container_bytes(
    data: bytes,
    rel_name: str,
    aes_key: Optional[bytes],
    wanted_exts: Tuple[str, ...],
) -> Iterable[Tuple[str, bytes]]:
    """Yield (virtual_path, bytes) recursively through RPF2 and IMG v3 containers."""
    low = rel_name.lower()
    if low.endswith(".rpf") or (len(data) >= 2048 and data[:4] == b"RPF2"):
        for sub_name, sub_data in iter_rpf2_files(data, aes_key, rel_name, wanted_exts=wanted_exts):
            yield from walk_container_bytes(sub_data, f"{rel_name}/{sub_name}", aes_key, wanted_exts)
    elif low.endswith(".img"):
        for sub_name, sub_data in iter_img3_files(data, aes_key, rel_name, wanted_exts=wanted_exts):
            yield from walk_container_bytes(sub_data, f"{rel_name}/{sub_name}", aes_key, wanted_exts)
    elif low.endswith(wanted_exts):
        yield rel_name, data


def _handle_host_file(
    p: Path,
    rel: str,
    aes_key: Optional[bytes],
    wanted_exts: Tuple[str, ...],
) -> Iterable[Tuple[str, bytes]]:
    low = p.name.lower()
    if low.endswith(".iso"):
        for sub_rel, data in iter_gdfx_iso_files(p, wanted_exts=wanted_exts):
            yield from walk_container_bytes(data, f"{rel}/{sub_rel}", aes_key, wanted_exts)
    elif low.endswith((".rpf", ".img", ".xtd", ".wtd", ".dds")):
        yield from walk_container_bytes(p.read_bytes(), rel, aes_key, wanted_exts)
    else:
        magic = p.open("rb").read(4)
        if magic in (b"LIVE", b"PIRS", b"CON "):
            for sub_rel, data in iter_stfs_files(p, wanted_exts=wanted_exts):
                yield from walk_container_bytes(data, f"{rel}/{sub_rel}", aes_key, wanted_exts)


def collect_inputs(
    paths: List[Path],
    aes_key: Optional[bytes],
    wanted_exts: Tuple[str, ...] = (".rpf", ".img", ".xtd", ".wtd", ".dds"),
) -> Iterable[Tuple[str, bytes]]:
    for root in paths:
        if root.is_file():
            yield from _handle_host_file(root, root.name, aes_key, wanted_exts)
        elif root.is_dir():
            for p in sorted(root.rglob("*")):
                if p.is_file():
                    yield from _handle_host_file(p, p.relative_to(root).as_posix(), aes_key, wanted_exts)


def index_xbox_textures(
    xbox_paths: List[Path],
    aes_key: Optional[bytes],
    dump_dir: Optional[Path] = None,
) -> Tuple[Dict[Tuple[str, str], List[XboxTextureEntry]], Dict[str, List[XboxTextureEntry]]]:
    by_dict_tex: Dict[Tuple[str, str], List[XboxTextureEntry]] = defaultdict(list)
    by_tex: Dict[str, List[XboxTextureEntry]] = defaultdict(list)
    xtd_count = 0
    tex_count = 0

    if dump_dir:
        dump_dir.mkdir(parents=True, exist_ok=True)

    for vpath, data in collect_inputs(xbox_paths, aes_key, wanted_exts=WANTED_XBOX_EXTS):
        if not vpath.lower().endswith(".xtd"):
            continue
        xtd_count += 1
        dict_name = Path(vpath).stem.lower()
        entries = parse_xtd_bytes(data, dict_name, keep_mip0=bool(dump_dir))
        for ent in entries:
            tex_count += 1
            by_dict_tex[(ent.dict_name, ent.tex_name)].append(ent)
            by_tex[ent.tex_name].append(ent)
            if dump_dir and ent.mip0_linear is not None:
                dds_fmt_map = {
                    XENOS_FMT_8: dict(fourcc=None, dxgi_format=61, rgb_bpp=8, is_compressed=False),
                    XENOS_FMT_8_8_8_8: dict(fourcc=None, dxgi_format=28, rgb_bpp=32, is_compressed=False),
                    XENOS_FMT_DXT1: dict(fourcc=b"DXT1", dxgi_format=None, is_compressed=True),
                    XENOS_FMT_DXT2_3: dict(fourcc=b"DXT3", dxgi_format=None, is_compressed=True),
                    XENOS_FMT_DXT4_5: dict(fourcc=b"DXT5", dxgi_format=None, is_compressed=True),
                    XENOS_FMT_DXN: dict(fourcc=None, dxgi_format=83, is_compressed=True),
                    XENOS_FMT_DXT5A: dict(fourcc=None, dxgi_format=80, is_compressed=True),
                }
                kw = dds_fmt_map[ent.base_format]
                out_dds = build_dds_bytes(ent.width, ent.height, 1, ent.mip0_linear, **kw)
                (dump_dir / f"{ent.hash_hex}.dds").write_bytes(out_dds)
                ent.mip0_linear = None

    print(f"[xbox] Indexed {tex_count} textures across {xtd_count} .xtd dictionaries.")
    return by_dict_tex, by_tex


def extract_mod_assets(mod_paths: List[Path], aes_key: Optional[bytes]) -> List[PcTextureAsset]:
    assets: List[PcTextureAsset] = []
    for vpath, data in collect_inputs(mod_paths, aes_key, wanted_exts=WANTED_MOD_EXTS):
        low = vpath.lower()
        if low.endswith(".wtd"):
            dict_name = Path(vpath).stem.lower()
            assets.extend(parse_wtd_bytes(data, dict_name, vpath))
        elif low.endswith(".dds"):
            p = Path(vpath)
            parent_low = p.parent.name.lower() if p.parent.name else ""
            dict_name = parent_low[:-4] if parent_low.endswith((".wtd", ".xtd")) else None
            assets.append(
                PcTextureAsset(
                    dict_name=dict_name,
                    tex_name=normalize_tex_name(p.name),
                    dds_bytes=data,
                    source_desc=vpath,
                )
            )
    return assets


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert PC GTA IV texture mods (.wtd/.img/.rpf/.dds) to PS5 <xxh64>.dds packs."
    )
    parser.add_argument(
        "--xbox",
        type=Path,
        action="append",
        required=True,
        help="Path to Xbox 360 game folder, xbox360.rpf, .img, or .xtd (repeatable)",
    )
    parser.add_argument(
        "--mod",
        type=Path,
        action="append",
        default=[],
        help="Path to a PC mod folder, .img, .wtd, or .dds pack (repeatable, later overrides earlier)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        required=True,
        help="Output folder for <xxh64>.dds replacement textures",
    )
    parser.add_argument(
        "--aes-key",
        type=Path,
        default=DEFAULT_AES_KEY_PATH if DEFAULT_AES_KEY_PATH.exists() else None,
        help="Path to 32-byte GTA IV AES-256-ECB key (defaults to LibertyRecompLib/aes_key.bin)",
    )
    parser.add_argument(
        "--max-size",
        type=int,
        default=0,
        help="Optional max width/height (e.g. 2048); down-caps textures with mipmaps losslessly",
    )
    parser.add_argument(
        "--allow-ambiguous",
        action="store_true",
        help="Fallback to matching by texture name even when multiple dictionaries use that name",
    )
    parser.add_argument(
        "--dump-xbox-dds",
        type=Path,
        default=None,
        help="Optional folder to also write decoded Xbox 360 mip0 <xxh64>.dds files",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Optional JSON report path listing matched hashes and unmatched PC textures",
    )
    args = parser.parse_args(argv)

    aes_key: Optional[bytes] = None
    if args.aes_key is not None and args.aes_key.exists():
        raw_key = args.aes_key.read_bytes()
        if len(raw_key) != 32:
            print(f"ERROR: AES key at {args.aes_key} must be 32 bytes (got {len(raw_key)})", file=sys.stderr)
            return 2
        aes_key = raw_key

    by_dict_tex, by_tex = index_xbox_textures(args.xbox, aes_key, dump_dir=args.dump_xbox_dds)
    if not by_tex:
        print("WARNING: No Xbox 360 textures indexed.", file=sys.stderr)

    args.out.mkdir(parents=True, exist_ok=True)

    written: Dict[str, Tuple[str, str]] = {}
    unmatched: List[str] = []
    ambiguous: List[str] = []

    for mod_path in args.mod:
        assets = extract_mod_assets([mod_path], aes_key)
        mod_matched = 0
        for asset in assets:
            targets: List[XboxTextureEntry] = []
            if asset.dict_name and (asset.dict_name, asset.tex_name) in by_dict_tex:
                targets = by_dict_tex[(asset.dict_name, asset.tex_name)]
            else:
                cands = by_tex.get(asset.tex_name, [])
                unique_hashes = {c.hash_hex for c in cands}
                if len(unique_hashes) == 1:
                    targets = cands
                elif len(unique_hashes) > 1:
                    if args.allow_ambiguous:
                        targets = cands
                    else:
                        ambiguous.append(f"{asset.source_desc} ({len(unique_hashes)} candidate hashes)")
                        continue
                else:
                    unmatched.append(asset.source_desc)
                    continue

            dds_out = downcap_dds_by_mips(asset.dds_bytes, args.max_size)
            for ent in targets:
                (args.out / f"{ent.hash_hex}.dds").write_bytes(dds_out)
                written[ent.hash_hex] = (f"{ent.dict_name}/{ent.tex_name}", asset.source_desc)
                mod_matched += 1

        print(f"[mod] {mod_path}: extracted {len(assets)} PC textures -> matched {mod_matched} Xbox entries.")

    print(
        f"[done] Wrote {len(written)} unique <hash>.dds file(s) to '{args.out}' "
        f"(unmatched={len(unmatched)}, ambiguous_skipped={len(ambiguous)})."
    )

    if args.report:
        report = {
            "written_count": len(written),
            "written": {k: {"xbox": v[0], "source": v[1]} for k, v in sorted(written.items())},
            "unmatched_count": len(unmatched),
            "unmatched": unmatched[:500],
            "ambiguous_count": len(ambiguous),
            "ambiguous": ambiguous[:500],
        }
        args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

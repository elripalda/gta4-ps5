#!/usr/bin/env python3
"""Render the original DualSense diagram to SVG, PNG and a 512x512 DXT5 DDS.

The rasteriser is deterministic and dependency-free (no Pillow/numpy): polygon
fills use scanline coverage, strokes use capsule distance fields, and the result
is supersampled before downsampling to 512x512. The BC3 encoder matches the
DXT5 format expected by frontend_controller_patch.cpp (linear layout; the PS5
port performs the Xbox 360 byte-swap/tiling at runtime).

Licensed GPL-3.0-or-later, same as the rest of this repository.
"""

import argparse
import hashlib
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dualsense_geometry as geom  # noqa: E402

SIZE = geom.CANVAS
DDS_HEADER_SIZE = 128
DDS_PAYLOAD_SIZE = SIZE * SIZE  # 262144 bytes for DXT5 512x512


# ---------------------------------------------------------------------------
# Rasteriser
# ---------------------------------------------------------------------------

def flatten(shape, steps=24):
    """Flatten a path into one or more polylines."""
    polys = []
    current = []
    pos = None
    start = None
    for cmd in shape:
        tag = cmd[0]
        if tag == "M":
            if current:
                polys.append(current)
            pos = (cmd[1], cmd[2])
            start = pos
            current = [pos]
        elif tag == "L":
            pos = (cmd[1], cmd[2])
            current.append(pos)
        elif tag == "C":
            x0, y0 = pos
            c1 = (cmd[1], cmd[2])
            c2 = (cmd[3], cmd[4])
            end = (cmd[5], cmd[6])
            for i in range(1, steps + 1):
                u = i / steps
                v = 1.0 - u
                a = v * v * v
                b = 3.0 * v * v * u
                c = 3.0 * v * u * u
                d = u * u * u
                current.append((
                    a * x0 + b * c1[0] + c * c2[0] + d * end[0],
                    a * y0 + b * c1[1] + c * c2[1] + d * end[1],
                ))
            pos = end
        elif tag == "Q":
            x0, y0 = pos
            control = (cmd[1], cmd[2])
            end = (cmd[3], cmd[4])
            for i in range(1, steps + 1):
                u = i / steps
                v = 1.0 - u
                current.append((
                    v * v * x0 + 2 * v * u * control[0] + u * u * end[0],
                    v * v * y0 + 2 * v * u * control[1] + u * u * end[1],
                ))
            pos = end
        elif tag == "Z":
            if current and start and current[-1] != start:
                current.append(start)
    if current:
        polys.append(current)
    return polys


def fill_polygons(buf, width, height, polys, color, ss):
    edges = []
    for poly in polys:
        pts = [(x * ss, y * ss) for x, y in poly]
        if len(pts) < 3:
            continue
        if pts[0] != pts[-1]:
            pts.append(pts[0])
        for i in range(len(pts) - 1):
            x0, y0 = pts[i]
            x1, y1 = pts[i + 1]
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    if not edges:
        return
    y_min = max(0, int(math.floor(min(min(e[1], e[3]) for e in edges))))
    y_max = min(height - 1, int(math.ceil(max(max(e[1], e[3]) for e in edges))))
    rgba = bytes((color[0], color[1], color[2], 255))
    for y in range(y_min, y_max + 1):
        sy = y + 0.5
        events = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= sy < y1) or (y1 <= sy < y0):
                t = (sy - y0) / (y1 - y0)
                x = x0 + t * (x1 - x0)
                events.append((x, 1 if y1 > y0 else -1))
        if len(events) < 2:
            continue
        events.sort()
        wind = 0
        row = y * width
        for i in range(len(events) - 1):
            wind += events[i][1]
            if wind == 0:
                continue
            xa, xb = events[i][0], events[i + 1][0]
            first = max(0, int(math.ceil(xa - 0.5)))
            last = min(width - 1, int(math.floor(xb - 0.5)))
            for x in range(first, last + 1):
                o = (row + x) * 4
                buf[o:o + 4] = rgba


def stroke_polylines(buf, width, height, polys, color, stroke_width, ss):
    radius = stroke_width * ss * 0.5
    radius_sq = radius * radius
    rgba = bytes((color[0], color[1], color[2], 255))
    for poly in polys:
        for i in range(len(poly) - 1):
            ax, ay = poly[i][0] * ss, poly[i][1] * ss
            bx, by = poly[i + 1][0] * ss, poly[i + 1][1] * ss
            min_x = max(0, int(math.floor(min(ax, bx) - radius)))
            max_x = min(width - 1, int(math.ceil(max(ax, bx) + radius)))
            min_y = max(0, int(math.floor(min(ay, by) - radius)))
            max_y = min(height - 1, int(math.ceil(max(ay, by) + radius)))
            dx = bx - ax
            dy = by - ay
            length_sq = dx * dx + dy * dy
            inv = 0.0 if length_sq == 0.0 else 1.0 / length_sq
            for y in range(min_y, max_y + 1):
                py = y + 0.5
                row = y * width
                for x in range(min_x, max_x + 1):
                    px = x + 0.5
                    if inv:
                        t = ((px - ax) * dx + (py - ay) * dy) * inv
                        if t < 0.0:
                            t = 0.0
                        elif t > 1.0:
                            t = 1.0
                    else:
                        t = 0.0
                    qx = ax + t * dx
                    qy = ay + t * dy
                    ddx = px - qx
                    ddy = py - qy
                    if ddx * ddx + ddy * ddy <= radius_sq:
                        o = (row + x) * 4
                        buf[o:o + 4] = rgba


def render(items, ss):
    width = height = SIZE * ss
    buf = bytearray(width * height * 4)
    for item in items:
        polys = flatten(item["shape"])
        if item["kind"] == "fill":
            fill_polygons(buf, width, height, polys, item["color"], ss)
        else:
            stroke_polylines(buf, width, height, polys, item["color"],
                             item["width"], ss)
    return buf


def downsample(buf, ss):
    out = bytearray(SIZE * SIZE * 4)
    src_width = SIZE * ss
    samples = ss * ss
    for y in range(SIZE):
        for x in range(SIZE):
            r = g = b = a = 0
            for dy in range(ss):
                base = ((y * ss + dy) * src_width + x * ss) * 4
                for dx in range(ss):
                    o = base + dx * 4
                    alpha = buf[o + 3]
                    if alpha:
                        r += buf[o] * alpha
                        g += buf[o + 1] * alpha
                        b += buf[o + 2] * alpha
                        a += alpha
            o = (y * SIZE + x) * 4
            if a:
                out[o] = (r + a // 2) // a
                out[o + 1] = (g + a // 2) // a
                out[o + 2] = (b + a // 2) // a
            out[o + 3] = (a + samples // 2) // samples
    return bytes(out)


# ---------------------------------------------------------------------------
# PNG / DDS encoding
# ---------------------------------------------------------------------------

def write_png(path, rgba):
    raw = bytearray()
    stride = SIZE * 4
    for y in range(SIZE):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        data = struct.pack(">I", len(payload)) + tag + payload
        return data + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def _rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _expand565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _palette4(c0, c1):
    p0, p1 = _expand565(c0), _expand565(c1)
    if c0 > c1:
        return [p0, p1,
                tuple((2 * p0[k] + p1[k]) // 3 for k in range(3)),
                tuple((p0[k] + 2 * p1[k]) // 3 for k in range(3))]
    return [p0, p1, tuple((p0[k] + p1[k]) // 2 for k in range(3)), (0, 0, 0)]


def _encode_color_block(pixels):
    r_min = min(p[0] for p in pixels)
    r_max = max(p[0] for p in pixels)
    g_min = min(p[1] for p in pixels)
    g_max = max(p[1] for p in pixels)
    b_min = min(p[2] for p in pixels)
    b_max = max(p[2] for p in pixels)
    c0 = _rgb565(r_max, g_max, b_max)
    c1 = _rgb565(r_min, g_min, b_min)
    if c0 < c1:
        c0, c1 = c1, c0
    candidates = [(c0, c1)]
    if c0 != c1:
        p0, p1 = _expand565(c0), _expand565(c1)
        candidates.append((_rgb565(*p0), _rgb565(*p1)))
    best = None
    for a, b in candidates:
        palette = _palette4(a, b)
        indices = []
        error = 0
        for p in pixels:
            best_distance = 1 << 30
            best_index = 0
            for i, q in enumerate(palette):
                d = ((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 +
                     (p[2] - q[2]) ** 2)
                if d < best_distance:
                    best_distance = d
                    best_index = i
            error += best_distance
            indices.append(best_index)
        if best is None or error < best[0]:
            best = (error, a, b, indices)
    _, a, b, indices = best
    bits = 0
    for position, index in enumerate(indices):
        bits |= index << (2 * position)
    return struct.pack("<HHI", a, b, bits)


def _encode_alpha_block(values):
    a0 = max(values)
    a1 = min(values)
    if a0 == a1:
        return bytes((a0, a1, 0, 0, 0, 0, 0, 0))
    palette = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    bits = 0
    for position, value in enumerate(values):
        best_distance = 1 << 30
        best_index = 0
        for i, q in enumerate(palette):
            d = (value - q) ** 2
            if d < best_distance:
                best_distance = d
                best_index = i
        bits |= best_index << (3 * position)
    return bytes((a0, a1)) + bits.to_bytes(6, "little")


def encode_bc3(rgba):
    blocks = SIZE // 4
    out = bytearray()
    for by in range(blocks):
        for bx in range(blocks):
            alphas = []
            colors = []
            for y in range(4):
                for x in range(4):
                    o = ((by * 4 + y) * SIZE + bx * 4 + x) * 4
                    r, g, b, a = rgba[o:o + 4]
                    alphas.append(a)
                    colors.append((r, g, b) if a else (0, 0, 0))
            out += _encode_alpha_block(alphas)
            out += _encode_color_block(colors)
    return bytes(out)


def dds_header(payload_size):
    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<I", header, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000)
    struct.pack_into("<II", header, 12, SIZE, SIZE)
    struct.pack_into("<I", header, 20, payload_size)
    struct.pack_into("<I", header, 24, 0)
    struct.pack_into("<I", header, 28, 0)
    struct.pack_into("<I", header, 76, 32)
    struct.pack_into("<I", header, 80, 0x4)
    header[84:88] = b"DXT5"
    struct.pack_into("<I", header, 108, 0x1000)
    return bytes(header)


# ---------------------------------------------------------------------------
# Verification helpers
# ---------------------------------------------------------------------------

def decode_dxt5(data):
    """Decode a linear DXT5 payload back to RGBA for round-trip checks."""
    assert data[:4] == b"DDS "
    height, width = struct.unpack_from("<II", data, 12)
    out = bytearray(width * height * 4)
    off = DDS_HEADER_SIZE
    bw, bh = (width + 3) // 4, (height + 3) // 4
    for by in range(bh):
        for bx in range(bw):
            a0, a1 = data[off], data[off + 1]
            alpha_bits = int.from_bytes(data[off + 2:off + 8], "little")
            c0, c1 = struct.unpack_from("<HH", data, off + 8)
            color_bits = int.from_bytes(data[off + 12:off + 16], "little")
            off += 16
            alpha = [a0, a1]
            if a0 > a1:
                alpha += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            else:
                alpha += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)]
                alpha += [0, 255]
            p0, p1 = _expand565(c0), _expand565(c1)
            palette = [p0, p1]
            if c0 > c1:
                palette += [tuple((2 * p0[k] + p1[k]) // 3 for k in range(3)),
                            tuple((p0[k] + 2 * p1[k]) // 3 for k in range(3))]
            else:
                palette += [tuple((p0[k] + p1[k]) // 2 for k in range(3)),
                            (0, 0, 0)]
            for py in range(4):
                for px in range(4):
                    i = py * 4 + px
                    x, y = bx * 4 + px, by * 4 + py
                    if x >= width or y >= height:
                        continue
                    r, g, b = palette[(color_bits >> (2 * i)) & 3]
                    o = (y * width + x) * 4
                    out[o] = r
                    out[o + 1] = g
                    out[o + 2] = b
                    out[o + 3] = alpha[(alpha_bits >> (3 * i)) & 7]
    return width, height, bytes(out)


def line_rows(rgba, x):
    rows = []
    for y in range(SIZE):
        o = (y * SIZE + x) * 4
        if rgba[o + 3] > 8:
            rows.append(y)
    groups = []
    for y in rows:
        if groups and y - groups[-1][-1] <= 2:
            groups[-1].append(y)
        else:
            groups.append([y])
    return [round((g[0] + g[-1]) / 2.0, 1) for g in groups]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--svg", help="write the editable SVG here")
    parser.add_argument("--png", help="write a transparent PNG here")
    parser.add_argument("--dds", help="write the 512x512 DXT5 DDS here")
    parser.add_argument("--ss", type=int, default=4,
                        help="supersampling factor (default 4)")
    parser.add_argument("--strict", action="store_true",
                        help="fail if leader-line rows drift")
    args = parser.parse_args()

    items = geom.build()

    if args.svg:
        with open(args.svg, "w", encoding="utf-8") as handle:
            handle.write(geom.to_svg())
        print("svg:", args.svg)

    if not (args.png or args.dds):
        return 0

    print("rasterising at %dx supersampling..." % args.ss)
    rgba = downsample(render(items, args.ss), args.ss)

    if args.png:
        write_png(args.png, rgba)
        print("png:", args.png)

    left = line_rows(rgba, 16)
    right = line_rows(rgba, 496)
    expected_left = list(geom.LEFT_ROWS)
    expected_right = list(geom.RIGHT_ROWS)
    print("left rows :", left)
    print("right rows:", right)

    def close(a, b):
        return len(a) == len(b) and all(abs(x - y) <= 1.0 for x, y in zip(a, b))

    ok = close(left, expected_left) and close(right, expected_right)
    print("row alignment:", "PASS" if ok else "FAIL")
    if args.strict and not ok:
        return 1

    if args.dds:
        payload = encode_bc3(rgba)
        assert len(payload) == DDS_PAYLOAD_SIZE, len(payload)
        data = dds_header(len(payload)) + payload
        with open(args.dds, "wb") as handle:
            handle.write(data)
        digest = hashlib.sha256(data).hexdigest()
        print("dds:", args.dds)
        print("dds sha256:", digest)
        print("dds bytes:", len(data))

        width, height, decoded = decode_dxt5(data)
        assert (width, height) == (SIZE, SIZE)
        total = peak = 0
        for i in range(0, len(rgba), 4):
            for k in range(4):
                diff = abs(rgba[i + k] - decoded[i + k])
                total += diff * diff
                peak = max(peak, diff)
        rmse = math.sqrt(total / len(rgba))
        print("round-trip rmse: %.3f peak: %d" % (rmse, peak))
        if args.strict and rmse > 6.0:
            return 1
        decoded_left = line_rows(decoded, 16)
        decoded_right = line_rows(decoded, 496)
        print("decoded rows L:", decoded_left)
        print("decoded rows R:", decoded_right)
        if args.strict and not (close(decoded_left, expected_left) and
                                close(decoded_right, expected_right)):
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

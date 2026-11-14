"""Original DualSense controller diagram for the GTA IV PS5 Controls screen.

Authored for this project by the LibertyRecomp PS5 contributors and licensed
GPL-3.0-or-later, matching the repository. This module is the single source of
truth for both the editable SVG and the 512x512 DXT5 texture export.

The drawing is a hand-authored front view of a DualSense controller. It is not
derived from, traced over, or recoloured from the imported FusionFix texture;
only the functional layout contract of the Controls screen (ten leader-line
rows on the left, nine on the right, at the fixed UI row positions) is
preserved so the surrounding text stays aligned.

Coordinates are in a 512x512 canvas, origin top-left, y down.
"""

CANVAS = 512

# Greys sampled from the GTA IV Controls screen so the new diagram sits in the
# same visual language. Colours themselves are not copyrightable.
BODY = (41, 41, 41)          # main shell
PANEL = (57, 58, 57)         # raised surfaces, buttons, sticks
OUTLINE = (156, 157, 156)    # main contour
DIM = (110, 111, 110)        # secondary contour
LIGHT = (185, 186, 185)      # glyphs and highlights
CALLOUT = (131, 129, 131)    # leader lines

# Controls-screen leader-line rows (512-space y). These are interface
# coordinates, not artwork: they must not move or the text rows drift.
LEFT_ROWS = (53.0, 80.0, 110.0, 136.0, 162.0, 189.5, 216.0, 243.0, 270.5, 297.5)
RIGHT_ROWS = (53.0, 80.0, 135.0, 162.5, 189.5, 216.5, 243.0, 270.5, 298.5)


def _rr(x0, y0, x1, y1, r):
    """Rounded rectangle as a cubic path."""
    k = 0.5522847498307936
    return [
        ("M", x0 + r, y0),
        ("L", x1 - r, y0),
        ("C", x1 - r + k * r, y0, x1, y0 + r - k * r, x1, y0 + r),
        ("L", x1, y1 - r),
        ("C", x1, y1 - r + k * r, x1 - r + k * r, y1, x1 - r, y1),
        ("L", x0 + r, y1),
        ("C", x0 + r - k * r, y1, x0, y1 - r + k * r, x0, y1 - r),
        ("L", x0, y0 + r),
        ("C", x0, y0 + r - k * r, x0 + r - k * r, y0, x0 + r, y0),
        ("Z",),
    ]


def _ellipse(cx, cy, rx, ry):
    k = 0.5522847498307936
    return [
        ("M", cx + rx, cy),
        ("C", cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry),
        ("C", cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy),
        ("C", cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry),
        ("C", cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy),
        ("Z",),
    ]


def _circle(cx, cy, r):
    return _ellipse(cx, cy, r, r)


def _poly(points):
    cmds = [("M", points[0][0], points[0][1])]
    for x, y in points[1:]:
        cmds.append(("L", x, y))
    cmds.append(("Z",))
    return cmds


def _map(shape, fn):
    out = []
    for cmd in shape:
        points = cmd[1:]
        moved = []
        for i in range(0, len(points), 2):
            x, y = fn(points[i], points[i + 1])
            moved.extend((x, y))
        out.append((cmd[0], *moved))
    return out


def mirror_x(shape):
    return _map(shape, lambda x, y: (CANVAS - x, y))


def rotate(shape, degrees, cx=0.0, cy=0.0):
    import math

    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)

    def fn(x, y):
        dx, dy = x - cx, y - cy
        return (cx + dx * c - dy * s, cy + dx * s + dy * c)

    return _map(shape, fn)


def translate(shape, dx, dy):
    return _map(shape, lambda x, y: (x + dx, y + dy))


def scale(shape, sx, sy, cx=0.0, cy=0.0):
    return _map(shape, lambda x, y: (cx + (x - cx) * sx, cy + (y - cy) * sy))


def reverse(shape):
    """Reverse an open path (M/L/C only), preserving the curve geometry."""
    segments = []
    current = None
    for cmd in shape:
        if cmd[0] == "M":
            current = (cmd[1], cmd[2])
        elif cmd[0] == "L":
            target = (cmd[1], cmd[2])
            segments.append(("L", current, target))
            current = target
        elif cmd[0] == "C":
            c1 = (cmd[1], cmd[2])
            c2 = (cmd[3], cmd[4])
            target = (cmd[5], cmd[6])
            segments.append(("C", current, c1, c2, target))
            current = target
    out = [("M", current[0], current[1])]
    for seg in reversed(segments):
        if seg[0] == "L":
            out.append(("L", seg[1][0], seg[1][1]))
        else:
            _, start, c1, c2, _ = seg
            out.append(("C", c2[0], c2[1], c1[0], c1[1], start[0], start[1]))
    return out


def _body_path():
    """Symmetric DualSense shell, 512-space.

    Top edge sits low between the shoulder bases; the two grips reach y~328 and
    the waist is widest (x~21..491) around y~245, matching the proportions the
    Controls screen was designed around.
    """
    left = [
        ("M", 256, 82),
        ("C", 230, 82, 206, 80, 184, 78),
        ("C", 164, 76, 150, 74, 140, 67),
        ("C", 131, 62, 120, 58, 108, 58),
        ("C", 96, 58, 88, 62, 82, 71),
        ("C", 74, 81, 66, 94, 58, 108),
        ("C", 46, 130, 34, 156, 28, 182),
        ("C", 22, 206, 20, 232, 22, 254),
        ("C", 23, 273, 27, 288, 34, 302),
        ("C", 42, 318, 56, 328, 74, 328),
        ("C", 90, 328, 102, 321, 110, 309),
        ("C", 116, 299, 120, 288, 124, 279),
        ("C", 130, 268, 139, 265, 152, 265),
        ("C", 174, 265, 192, 266, 212, 267),
        ("C", 227, 268, 242, 268, 256, 268),
    ]
    right = mirror_x(reverse(left))
    # right starts where left ended (bottom centre); continue up the right side.
    return left + right[1:] + [("Z",)]


def _dpad_petals():
    up = _poly([(92, 171), (110, 171), (110, 157), (101, 147), (92, 157)])
    down = _poly([(92, 189), (110, 189), (110, 203), (101, 213), (92, 203)])
    left = _poly([(93, 171), (93, 189), (80, 189), (70, 180), (80, 171)])
    right = _poly([(109, 171), (109, 189), (122, 189), (132, 180), (122, 171)])
    return up, down, left, right


def _glyph_triangle(cx, cy, h=17):
    return _poly([(cx, cy - h / 2), (cx + h / 2, cy + h / 2), (cx - h / 2, cy + h / 2)])


def _glyph_cross(cx, cy, s=7):
    return [("M", cx - s, cy - s), ("L", cx + s, cy + s), ("M", cx + s, cy - s),
            ("L", cx - s, cy + s)]


def _glyph_square(cx, cy, s=7):
    return _rr(cx - s, cy - s, cx + s, cy + s, 1.5)


def _glyph_ps(cx, cy):
    """Simplified PS monogram drawn as strokes (original, not traced)."""
    return [
        # P stem and bowl
        ("M", cx - 5, cy - 7), ("L", cx - 5, cy + 8),
        ("M", cx - 5, cy - 7),
        ("C", cx + 1, cy - 8, cx + 2, cy - 1, cx - 5, cy - 1),
        # S curve
        ("M", cx + 5, cy - 6),
        ("C", cx - 1, cy - 7, cx - 2, cy - 2, cx + 3, cy + 0),
        ("C", cx + 7, cy + 2, cx + 6, cy + 7, cx - 0, cy + 8),
    ]


def _dpad():
    up, down, left, right = _dpad_petals()
    items = []
    for petal in (up, down, left, right):
        items.append({"kind": "fill", "shape": petal, "color": BODY, "group": "dpad"})
        items.append({"kind": "stroke", "shape": petal, "color": OUTLINE,
                      "width": 1.8, "group": "dpad"})
    arrows = [
        _poly([(101, 154), (97, 161), (105, 161)]),
        _poly([(101, 206), (97, 199), (105, 199)]),
        _poly([(76, 180), (83, 176), (83, 184)]),
        _poly([(126, 180), (119, 176), (119, 184)]),
    ]
    for arrow in arrows:
        items.append({"kind": "stroke", "shape": arrow, "color": LIGHT,
                      "width": 1.6, "group": "dpad"})
    return items


def _face_buttons():
    items = []
    centers = {
        "triangle": (411, 150),
        "circle": (441, 180),
        "cross": (411, 210),
        "square": (381, 180),
    }
    for name, (cx, cy) in centers.items():
        button = _circle(cx, cy, 14)
        items.append({"kind": "fill", "shape": button, "color": PANEL,
                      "group": "face-buttons"})
        items.append({"kind": "stroke", "shape": button, "color": OUTLINE,
                      "width": 1.8, "group": "face-buttons"})
        if name == "triangle":
            glyph = _glyph_triangle(cx, cy, 17)
        elif name == "circle":
            glyph = _circle(cx, cy, 7)
        elif name == "cross":
            glyph = _glyph_cross(cx, cy, 7)
        else:
            glyph = _glyph_square(cx, cy, 7)
        items.append({"kind": "stroke", "shape": glyph, "color": LIGHT,
                      "width": 2.0, "group": "face-buttons"})
    return items


def _stick(cx, cy, group="sticks"):
    items = []
    well = _circle(cx, cy, 39)
    items.append({"kind": "fill", "shape": well, "color": BODY, "group": group})
    items.append({"kind": "stroke", "shape": well, "color": OUTLINE,
                  "width": 1.8, "group": group})
    ring = _circle(cx, cy, 31)
    items.append({"kind": "stroke", "shape": ring, "color": DIM,
                  "width": 1.4, "group": group})
    dome = _circle(cx, cy + 1, 26)
    items.append({"kind": "fill", "shape": dome, "color": PANEL, "group": group})
    items.append({"kind": "stroke", "shape": dome, "color": OUTLINE,
                  "width": 1.6, "group": group})
    inner = _circle(cx, cy + 1, 19)
    items.append({"kind": "stroke", "shape": inner, "color": DIM,
                  "width": 1.2, "group": group})
    dot = _circle(cx, cy + 1, 5)
    items.append({"kind": "stroke", "shape": dot, "color": DIM,
                  "width": 1.2, "group": group})
    return items


def _callouts():
    """Leader lines. Row y-values are fixed; routing is authored here.

    Each polyline starts at the texture edge (x=14 left, x=498 right) and ends
    on the control it labels. Round caps keep the endpoints tidy.
    """
    left = [
        [(14, 53.0), (108, 53.0)],
        [(14, 80.0), (96, 80.0), (96, 87.0)],
        [(14, 110.0), (125, 110.0), (125, 124.0), (141, 124.0)],
        [(14, 136.0), (62, 136.0), (62, 152.0), (97, 152.0)],
        [(14, 162.0), (66, 162.0), (66, 180.0), (70, 180.0)],
        [(14, 189.5), (48, 189.5), (48, 203.0), (92, 203.0)],
        [(14, 216.0), (130, 216.0), (130, 212.0), (176, 212.0)],
        [(14, 243.0), (144, 243.0)],
        [(14, 270.5), (104, 270.5), (104, 281.0), (177, 281.0)],
        [(14, 297.5), (58, 297.5), (58, 290.0), (80, 290.0)],
    ]
    right = [
        [(498, 53.0), (416, 53.0)],
        [(498, 80.0), (428, 80.0), (428, 87.0)],
        [(498, 135.0), (448, 135.0), (448, 152.0), (427, 152.0)],
        [(498, 162.5), (466, 162.5), (466, 171.0), (451, 171.0)],
        [(498, 189.5), (474, 189.5), (474, 201.0), (424, 201.0)],
        [(498, 216.5), (396, 216.5), (396, 212.0), (340, 212.0)],
        [(498, 243.0), (390, 243.0), (390, 196.0), (381, 196.0)],
        [(498, 270.5), (430, 270.5), (430, 281.0), (338, 281.0)],
        [(498, 298.5), (464, 298.5), (464, 197.0), (356, 197.0)],
    ]
    items = []
    for line in left + right:
        # Half-pixel offset puts the 2.4px stroke on the same pixel rows the
        # retail layout uses, so the UI text rows do not drift.
        line = [(x, y + 0.5) for x, y in line]
        shape = [("M", line[0][0], line[0][1])]
        for x, y in line[1:]:
            shape.append(("L", x, y))
        items.append({"kind": "stroke", "shape": shape, "color": CALLOUT,
                      "width": 2.4, "group": "leader-lines"})
    return items


def build():
    """Return the paint-ordered list of drawing items."""
    items = []

    # Triggers sit behind the shell, only their crowns show above the shoulders.
    l2 = translate(rotate(_rr(-38, -9, 38, 9, 9), -8), 110, 57)
    r2 = mirror_x(l2)
    for cap in (l2, r2):
        items.append({"kind": "fill", "shape": cap, "color": BODY,
                      "group": "shoulder-controls"})
        items.append({"kind": "stroke", "shape": cap, "color": OUTLINE,
                      "width": 1.6, "group": "shoulder-controls"})

    body = _body_path()
    items.append({"kind": "fill", "shape": body, "color": BODY,
                  "group": "controller-body"})
    items.append({"kind": "stroke", "shape": body, "color": OUTLINE,
                  "width": 2.0, "group": "controller-body"})
    # Inner bevel following the shell contour.
    inner_body = scale(body, 0.965, 0.965, 256, 270)
    items.append({"kind": "stroke", "shape": inner_body, "color": DIM,
                  "width": 1.2, "group": "controller-body"})

    # Bumpers on the front of each shoulder.
    l1 = translate(rotate(_rr(-44, -15, 44, 15, 15), -8), 112, 76)
    r1 = mirror_x(l1)
    for pad in (l1, r1):
        items.append({"kind": "fill", "shape": pad, "color": PANEL,
                      "group": "shoulder-controls"})
        items.append({"kind": "stroke", "shape": pad, "color": OUTLINE,
                      "width": 1.9, "group": "shoulder-controls"})
        seam = translate(rotate(_ellipse(0, 0, 34, 9), -8), 114, 76)
        if pad is r1:
            seam = mirror_x(seam)
        items.append({"kind": "stroke", "shape": seam, "color": DIM,
                      "width": 1.2, "group": "shoulder-controls"})

    # Touchpad with its two light-bar strips.
    touchpad = _rr(160, 98, 352, 200, 14)
    items.append({"kind": "fill", "shape": touchpad, "color": PANEL,
                  "group": "touchpad"})
    items.append({"kind": "stroke", "shape": touchpad, "color": OUTLINE,
                  "width": 2.0, "group": "touchpad"})
    items.append({"kind": "stroke", "shape": _rr(166, 104, 346, 194, 10),
                  "color": DIM, "width": 1.2, "group": "touchpad"})
    for x0 in (151, 355):
        strip = _rr(x0, 126, x0 + 6, 172, 3)
        items.append({"kind": "fill", "shape": strip, "color": DIM,
                      "group": "touchpad"})

    # Create and Options buttons.
    create = _rr(140, 126, 153, 151, 6.5)
    options = _rr(359, 126, 372, 151, 6.5)
    for pill in (create, options):
        items.append({"kind": "fill", "shape": pill, "color": BODY,
                      "group": "system-buttons"})
        items.append({"kind": "stroke", "shape": pill, "color": OUTLINE,
                      "width": 1.4, "group": "system-buttons"})
    for a, b in (((146, 122), (141, 116)), ((146, 122), (146, 114)),
                 ((146, 122), (151, 116))):
        items.append({"kind": "stroke", "shape": [("M", *a), ("L", *b)],
                      "color": LIGHT, "width": 1.4, "group": "system-buttons"})
    for y in (113, 118, 123):
        items.append({"kind": "stroke",
                      "shape": [("M", 360, y), ("L", 371, y)],
                      "color": LIGHT, "width": 1.4, "group": "system-buttons"})

    items.extend(_dpad())
    items.extend(_face_buttons())
    items.extend(_stick(183, 244))
    items.extend(_stick(329, 244))

    # PS button and mute key.
    ps_button = _circle(256, 236, 14)
    items.append({"kind": "fill", "shape": ps_button, "color": PANEL,
                  "group": "system-buttons"})
    items.append({"kind": "stroke", "shape": ps_button, "color": OUTLINE,
                  "width": 1.4, "group": "system-buttons"})
    items.append({"kind": "stroke", "shape": _glyph_ps(256, 236), "color": LIGHT,
                  "width": 1.5, "group": "system-buttons"})
    mute = _rr(247, 209, 265, 218, 4.5)
    items.append({"kind": "fill", "shape": mute, "color": BODY,
                  "group": "system-buttons"})
    items.append({"kind": "stroke", "shape": mute, "color": OUTLINE,
                  "width": 1.2, "group": "system-buttons"})

    items.extend(_callouts())
    return items


def _fmt(v):
    text = f"{v:.2f}".rstrip("0").rstrip(".")
    return text if text not in ("-0", "") else "0"


def _path_d(shape):
    parts = []
    for cmd in shape:
        if cmd[0] == "Z":
            parts.append("Z")
            continue
        coords = " ".join(_fmt(v) for v in cmd[1:])
        parts.append(f"{cmd[0]}{coords}")
    return " ".join(parts)


def to_svg():
    """Serialise the model as an editable SVG document."""
    groups = [
        ("shoulder-controls", "Shoulder controls (L2/R2, L1/R1)"),
        ("controller-body", "Controller shell"),
        ("touchpad", "Touchpad and light bar"),
        ("system-buttons", "Create, Options, PS and mute buttons"),
        ("dpad", "Directional pad"),
        ("face-buttons", "Face buttons"),
        ("sticks", "Analog sticks"),
        ("leader-lines", "Controls-screen leader lines"),
    ]
    by_group = {gid: [] for gid, _ in groups}
    for item in build():
        by_group[item["group"]].append(item)

    def rgb(color):
        return "#%02X%02X%02X" % color

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512"',
        '     viewBox="0 0 512 512" version="1.1">',
        '  <title>DualSense controller diagram</title>',
        '  <desc>Original front-view diagram authored for the GTA IV PS5 Controls',
        '        screen. Licensed GPL-3.0-or-later.</desc>',
    ]
    for gid, label in groups:
        lines.append(f'  <g id="{gid}" data-label="{label}">')
        for item in by_group[gid]:
            d = _path_d(item["shape"])
            fill = "none" if item["kind"] == "stroke" else rgb(item["color"])
            attrs = [f'd="{d}"', f'fill="{fill}"']
            if item["kind"] == "stroke":
                attrs += [
                    f'stroke="{rgb(item["color"])}"',
                    f'stroke-width="{_fmt(item["width"])}"',
                    'stroke-linecap="round"',
                    'stroke-linejoin="round"',
                ]
            else:
                attrs.append('fill-rule="nonzero"')
            lines.append("    <path " + " ".join(attrs) + "/>")
        lines.append("  </g>")
    lines.append("</svg>")
    return "\n".join(lines) + "\n"

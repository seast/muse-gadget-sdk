#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Regenerate the clock screen's digit fonts and weather icons.

    python3 tools/muse/gen_clock_assets.py [--font Oswald.ttf]

Writes components/muse/fonts/muse_font_clock_*.c (Oswald Bold digits, 4 bpp,
the same width each) and components/muse/muse_clock_art.c (the icons, ARGB8888).
Without --font it downloads Oswald from github.com/google/fonts. Needs Pillow.
"""
import argparse
import math
import os
import tempfile
import urllib.request

from PIL import Image, ImageDraw, ImageFont

OSWALD_URL = "https://github.com/google/fonts/raw/main/ofl/oswald/Oswald%5Bwght%5D.ttf"
HERE = os.path.dirname(os.path.abspath(__file__))
MUSE = os.path.join(HERE, "..", "..", "components", "muse")
FIRST, LAST = 0x2D, 0x3A
FONT_SIZES = (40, 64, 84)
ICON_PX = 48
BADGE_PX = 28
SS = 4


def load_font(path, size):
    f = ImageFont.truetype(path, size)
    f.set_variation_by_axes([700])
    return f


def glyphs(font):
    ascent, _ = font.getmetrics()
    out = {}
    for cp in range(FIRST, LAST + 1):
        im, (ox, oy) = font.getmask2(chr(cp), mode="L")
        im = Image.frombytes("L", im.size, bytes(im))
        bb = im.getbbox() or (0, 0, 0, 0)
        crop = im.crop(bb)
        out[cp] = {
            "img": crop,
            "x": ox + bb[0],
            "top": ascent - (oy + bb[1]),
            "bottom": ascent - (oy + bb[3]),
            "adv": font.getlength(chr(cp)),
        }
    return out


def pack4(img):
    vals = [(v + 8) // 17 for v in img.tobytes()]
    if len(vals) % 2:
        vals.append(0)
    return bytes((vals[i] << 4) | vals[i + 1] for i in range(0, len(vals), 2))


def write_font(path, size, name):
    g = glyphs(load_font(path, size))
    digit_w = max(g[cp]["img"].width for cp in range(0x30, 0x3A))
    gap = max(2, size // 24)
    tab = digit_w + gap
    bitmap = bytearray()
    dscs = []
    top = max(v["top"] for v in g.values())
    bottom = min(v["bottom"] for v in g.values())
    for cp in range(FIRST, LAST + 1):
        v = g[cp]
        w, h = v["img"].size
        if 0x30 <= cp <= 0x39 or cp == 0x2D:
            adv = tab
            ofs_x = (tab - w) // 2
        else:
            adv = w + 2 * gap
            ofs_x = gap
        dscs.append((len(bitmap), adv * 16, w, h, ofs_x, v["bottom"]))
        bitmap += pack4(v["img"])
    rows = []
    for i in range(0, len(bitmap), 16):
        rows.append("    " + ", ".join("0x%02x" % b for b in bitmap[i:i + 16]) + ",")
    dsc_rows = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    for d in dscs:
        dsc_rows.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}," % d)
    text = f"""/*******************************************************************************
 * Size: {size} px
 * Bpp: 4
 * Opts: tools/muse/gen_clock_assets.py, Oswald Bold, U+{FIRST:04X}-U+{LAST:04X}, digits {tab} px wide
 ******************************************************************************/

#include "lvgl.h"

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{chr(10).join(rows)}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{chr(10).join(dsc_rows)}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = {FIRST}, .range_length = {LAST - FIRST + 1}, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }}
}};

static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {top - bottom},
    .base_line = {-bottom},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
}};
"""
    with open(os.path.join(MUSE, "fonts", name + ".c"), "w") as f:
        f.write(text)
    print("wrote %s: %d px tall, digits %d px wide, %d bytes" % (name, top - bottom, tab, len(bitmap)))


YELLOW = (247, 197, 49, 255)
MOON = (240, 228, 170, 255)
CLOUD = (214, 219, 228, 255)
DARK_CLOUD = (150, 158, 172, 255)
BLUE = (90, 169, 245, 255)
WHITE = (240, 244, 250, 255)
FOG = (170, 176, 188, 255)
BADGE = (44, 47, 56, 255)
RED = (230, 76, 76, 255)
DROP = (70, 155, 240, 255)


def canvas(px):
    return Image.new("RGBA", (px * SS, px * SS), (0, 0, 0, 0))


def disc(d, cx, cy, r, fill):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)


def stroke(d, a, b, w, fill):
    d.line((a, b), fill=fill, width=int(w))
    disc(d, a[0], a[1], w / 2, fill)
    disc(d, b[0], b[1], w / 2, fill)


def sun(im, cx, cy, r):
    d = ImageDraw.Draw(im)
    disc(d, cx, cy, r, YELLOW)
    for i in range(8):
        a = i * math.pi / 4
        p = (cx + math.cos(a) * r * 1.45, cy + math.sin(a) * r * 1.45)
        q = (cx + math.cos(a) * r * 1.95, cy + math.sin(a) * r * 1.95)
        stroke(d, p, q, r * 0.28, YELLOW)


def moon(im, cx, cy, r):
    mask = Image.new("L", im.size, 0)
    d = ImageDraw.Draw(mask)
    disc(d, cx, cy, r, 255)
    disc(d, cx + r * 0.55, cy - r * 0.35, r * 0.85, 0)
    im.paste(Image.new("RGBA", im.size, MOON), (0, 0), mask)


def cloud(im, cx, cy, w, fill):
    mask = Image.new("L", im.size, 0)
    d = ImageDraw.Draw(mask)
    h = w * 0.36
    d.rounded_rectangle((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), radius=h / 2, fill=255)
    disc(d, cx - w * 0.16, cy - h * 0.45, w * 0.22, 255)
    disc(d, cx + w * 0.14, cy - h * 0.62, w * 0.28, 255)
    im.paste(Image.new("RGBA", im.size, fill), (0, 0), mask)


def icon(kind):
    S = ICON_PX * SS
    im = canvas(ICON_PX)
    d = ImageDraw.Draw(im)
    if kind == "sun":
        sun(im, S / 2, S / 2, S * 0.2)
    elif kind == "moon":
        moon(im, S / 2, S / 2, S * 0.34)
    elif kind in ("sun_cloud", "moon_cloud"):
        if kind == "sun_cloud":
            sun(im, S * 0.36, S * 0.36, S * 0.14)
        else:
            moon(im, S * 0.38, S * 0.36, S * 0.22)
        cloud(im, S * 0.58, S * 0.66, S * 0.62, CLOUD)
    elif kind == "cloud":
        cloud(im, S / 2, S * 0.6, S * 0.82, CLOUD)
    elif kind == "fog":
        cloud(im, S / 2, S * 0.4, S * 0.7, CLOUD)
        for i, (x0, x1) in enumerate(((0.16, 0.74), (0.28, 0.86), (0.2, 0.66))):
            y = S * (0.68 + i * 0.11)
            stroke(d, (S * x0, y), (S * x1, y), S * 0.06, FOG)
    elif kind == "rain":
        cloud(im, S / 2, S * 0.42, S * 0.8, CLOUD)
        for x in (0.32, 0.5, 0.68):
            stroke(d, (S * x, S * 0.68), (S * (x - 0.06), S * 0.86), S * 0.06, BLUE)
    elif kind == "snow":
        cloud(im, S / 2, S * 0.42, S * 0.8, CLOUD)
        for x, y in ((0.32, 0.72), (0.5, 0.84), (0.68, 0.72)):
            for a in range(3):
                t = a * math.pi / 3
                dx, dy = math.cos(t) * S * 0.06, math.sin(t) * S * 0.06
                stroke(d, (S * x - dx, S * y - dy), (S * x + dx, S * y + dy), S * 0.03, WHITE)
    elif kind == "storm":
        cloud(im, S / 2, S * 0.4, S * 0.8, DARK_CLOUD)
        bolt = [(0.54, 0.52), (0.38, 0.76), (0.5, 0.76), (0.44, 0.96), (0.64, 0.68), (0.52, 0.68), (0.6, 0.52)]
        d.polygon([(S * x, S * y) for x, y in bolt], fill=YELLOW)
    return im.resize((ICON_PX, ICON_PX), Image.LANCZOS)


def badge(kind):
    S = BADGE_PX * SS
    im = canvas(BADGE_PX)
    d = ImageDraw.Draw(im)
    disc(d, S / 2, S / 2, S / 2, BADGE)
    if kind == "temp":
        d.rounded_rectangle((S * 0.43, S * 0.2, S * 0.57, S * 0.66), radius=S * 0.07, fill=RED)
        disc(d, S / 2, S * 0.68, S * 0.13, RED)
    else:
        disc(d, S / 2, S * 0.6, S * 0.17, DROP)
        d.polygon([(S / 2, S * 0.2), (S * 0.345, S * 0.53), (S * 0.655, S * 0.53)], fill=DROP)
    return im.resize((BADGE_PX, BADGE_PX), Image.LANCZOS)


LICENSE = """/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
"""

ICONS = ("sun", "moon", "sun_cloud", "moon_cloud", "cloud", "fog", "rain", "snow", "storm")
BADGES = ("temp", "humidity")


def c_image(name, im):
    data = bytearray()
    raw = im.tobytes()
    for i in range(0, len(raw), 4):
        data += bytes((raw[i + 2], raw[i + 1], raw[i], raw[i + 3]))
    rows = ["    " + ", ".join("0x%02x" % v for v in data[i:i + 16]) + "," for i in range(0, len(data), 16)]
    w, h = im.size
    return f"""static const uint8_t {name}_px[] = {{
{chr(10).join(rows)}
}};

const lv_image_dsc_t {name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = {w},
        .h = {h},
        .stride = {w * 4},
    }},
    .data_size = sizeof({name}_px),
    .data = {name}_px,
}};
"""


def write_art():
    parts = []
    for k in ICONS:
        parts.append(c_image("muse_clock_icon_" + k, icon(k)))
    for k in BADGES:
        parts.append(c_image("muse_clock_badge_" + k, badge(k)))
    with open(os.path.join(MUSE, "muse_clock_art.c"), "w") as f:
        f.write(LICENSE + "\n#include \"muse_clock_art.h\"\n\n" + "\n".join(parts))
    print("wrote muse_clock_art.c: %d icons, %d badges" % (len(ICONS), len(BADGES)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font")
    args = ap.parse_args()
    path = args.font
    if not path:
        path = os.path.join(tempfile.mkdtemp(), "Oswald.ttf")
        urllib.request.urlretrieve(OSWALD_URL, path)
    for size in FONT_SIZES:
        write_font(path, size, "muse_font_clock_%d" % size)
    write_art()


if __name__ == "__main__":
    main()

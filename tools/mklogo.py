"""Make the AGA-PISTORM line of the Emu68 boot splash.

    python tools/mklogo.py                       write include/logo/logo_aga.h
    python tools/mklogo.py --preview out.png     and a picture of the whole splash

Emu68 draws its boot screen from greyscale run-length images (the Emu68 logo,
then the PiStorm logo under it). This renders a line of text in the same
greyscale - background 120, ink dark - and encodes it in the same format, so
display_logo() can draw it under the PiStorm logo like the others. The preview
decodes all three and colours them exactly as draw_logo() does, including
CaffeineOS's logo=purple.

RLE format (raspi/start_rpi64.c rle_decode): width and height as 7-bit groups,
high first, bit 7 = more follows. Then runs: first byte bit 6 = literal, bit 7 =
count continues, bits 0-5 = the count's top bits; each continuation byte adds 7
more. A literal run is followed by `count` pixel bytes, a repeat by one.
SPDX-License-Identifier: GPL-2.0-only
"""
import os
import re
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO_DIR = os.path.join(ROOT, "third_party", "Emu68", "include", "logo")
TEXT = "AGA-Pistorm for ECS"
# The Emu68 and PiStorm logos are filled with grey 74 (peach under logo=purple)
# on 120, with darker edges; the shadow value turns dark purple under it.
BG, INK, SHADOW = 120, 74, 165


def render(text, height=56, size=34):
    fp = FontProperties(family="DejaVu Sans", weight="bold")
    dpi = 100
    fig = plt.figure(figsize=(1200 / dpi, height / dpi), dpi=dpi)
    fig.patch.set_facecolor((0, 0, 0))
    fig.text(0.5, 0.5, text, fontproperties=fp, fontsize=size, color=(1, 1, 1),
             ha="center", va="center")
    fig.canvas.draw()
    alpha = np.asarray(fig.canvas.buffer_rgba())[:, :, 0].astype(np.float32) / 255.0
    plt.close(fig)
    shadow = np.zeros_like(alpha)
    shadow[2:, 2:] = alpha[:-2, :-2]
    img = BG + (SHADOW - BG) * shadow
    img = img + (INK - img) * alpha
    img = np.rint(img).astype(np.int32)
    cols = np.where((img != BG).any(axis=0))[0]
    pad = 8
    img = img[:, max(cols[0] - pad, 0):cols[-1] + pad + 1]
    return np.clip(img, 0, 255).astype(np.uint8)


def varint(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def count_bytes(n, literal):
    groups = []
    while n >= 64 or (groups and n):
        groups.append(n & 0x7F)
        n >>= 7
    first = (0x40 if literal else 0) | (n & 0x3F) | (0x80 if groups else 0)
    rest = [g | 0x80 for g in reversed(groups[1:])] + [groups[0]] if groups else []
    return bytes([first] + rest)


def encode(img):
    h, w = img.shape
    px = img.flatten().tolist()
    out = bytearray(varint(w) + varint(h))
    i, n = 0, len(px)
    lit = []
    def flush():
        if lit:
            out.extend(count_bytes(len(lit), True))
            out.extend(lit)
            lit.clear()
    while i < n:
        j = i
        while j < n and px[j] == px[i]:
            j += 1
        if j - i >= 3:
            flush()
            out.extend(count_bytes(j - i, False))
            out.append(px[i])
        else:
            lit.extend(px[i:j])
        i = j
    flush()
    return bytes(out)


def decode(data):
    p = 0
    w = h = 0
    while True:
        w = (w << 7) | (data[p] & 0x7F); p += 1
        if not data[p - 1] & 0x80:
            break
    while True:
        h = (h << 7) | (data[p] & 0x7F); p += 1
        if not data[p - 1] & 0x80:
            break
    out = []
    while p < len(data) and len(out) < w * h:
        c = data[p]; p += 1
        literal = c & 0x40
        cnt = c & 0x3F
        while c & 0x80:
            cnt <<= 7
            c = data[p]; p += 1
            cnt |= c & 0x7F
        if literal:
            out.extend(data[p:p + cnt]); p += cnt
        else:
            out.extend([data[p]] * cnt); p += 1
    return np.array(out[:w * h], dtype=np.uint8).reshape(h, w)


def read_header(name):
    txt = open(os.path.join(LOGO_DIR, name + ".h")).read()
    body = txt[txt.index("{") + 1:txt.index("}")]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


def purple(g):
    g = 240 - g.astype(np.int32)
    r = np.clip(-330 + (g * 848 >> 8), 0, 255)
    gg = np.clip(-343 + (g * 768 >> 8), 0, 255)
    b = np.clip(-91 + (g * 341 >> 8), 0, 255)
    return np.stack([r, gg, b], -1).astype(np.uint8)


def main():
    img = render(TEXT)
    data = encode(img)
    assert (decode(data) == img).all(), "RLE round trip failed"
    # rle_decode() advances past the width/height header and then loops while
    # pos < length, counting from AFTER it - so the length it wants is the
    # payload alone. Emu68's own logos pass the whole array and read 2-4 bytes
    # past its end; this one stops exactly at the last byte.
    hdr = len(varint(img.shape[1])) + len(varint(img.shape[0]))
    out = os.path.join(LOGO_DIR, "logo_aga.h")
    with open(out, "w", newline="\n") as f:
        f.write("/* AGA-PISTORM splash line, generated by tools/mklogo.py - do not edit */\n")
        f.write("unsigned char logo_aga[] = {\n")
        for k in range(0, len(data), 12):
            f.write("  " + ", ".join("0x%02x" % b for b in data[k:k + 12]) + ",\n")
        f.write("};\n/* payload only: the %d header bytes are not counted */\n" % hdr)
        f.write("unsigned int logo_aga_len = %d;\n" % (len(data) - hdr))
    print("%s: %dx%d, %d bytes" % (out, img.shape[1], img.shape[0], len(data)))

    if "--preview" in sys.argv:
        path = sys.argv[sys.argv.index("--preview") + 1]
        emu, pis = decode(read_header("logo_emu68")), decode(read_header("logo_pistorm"))
        W, H = 1920, 1080
        canvas = np.full((H, W), emu[0, 0], dtype=np.uint8)
        y = (H - emu.shape[0]) // 2 - (pis.shape[0] + 10) // 2 - (img.shape[0] + 10) // 2
        for part in (emu, pis, img):
            x = (W - part.shape[1]) // 2
            canvas[y:y + part.shape[0], x:x + part.shape[1]] = part
            y += part.shape[0] + 10
        plt.imsave(path, purple(canvas))
        print("preview:", path)


if __name__ == "__main__":
    main()

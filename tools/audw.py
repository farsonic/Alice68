"""Decode agastat AUDW dumps: every write a game made to the sound registers.

    python tools/audw.py audw.bin            per-channel story + summary
    python tools/audw.py audw.bin --raw      every write, one per line

Entry layout (12 bytes, big-endian), from arm/aga_diag.c aga_diag_audw():
    size<<28 | frame,   vpos<<20 | hpos<<10 | reg,   value
SPDX-License-Identifier: GPL-2.0-only
"""
import struct
import sys
from collections import Counter, defaultdict

SUB = {0x0: "LCH", 0x2: "LCL", 0x4: "LEN", 0x6: "PER", 0x8: "VOL", 0xA: "DAT"}


def regname(reg):
    if 0x0A0 <= reg < 0x0E0:
        return "AUD%d%s" % ((reg - 0x0A0) >> 4, SUB.get(reg & 0xE, "?%X" % (reg & 0xE)))
    return {0x096: "DMACON", 0x09A: "INTENA", 0x09C: "INTREQ"}.get(reg, "$%03X" % reg)


def words(entries):
    """Split every write into the 16-bit register writes the chipset saw."""
    for frame, vpos, hpos, size, reg, value in entries:
        if size == 1:
            b = value & 0xFF
            yield frame, vpos, hpos, reg & 0x1FE, b | (b << 8)
        elif size == 4:
            yield frame, vpos, hpos, reg, (value >> 16) & 0xFFFF
            yield frame, vpos, hpos, reg + 2, value & 0xFFFF
        else:
            yield frame, vpos, hpos, reg, value & 0xFFFF


def load(path):
    data = open(path, "rb").read()
    out = []
    for i in range(0, len(data) - 11, 12):
        a, b, c = struct.unpack(">III", data[i:i + 12])
        out.append((a & 0x0FFFFFFF, (b >> 20) & 0x1FF, (b >> 10) & 0xFF, a >> 28, b & 0x1FF, c))
    return out


def bits(v, base):
    return "".join(str(n) if v & (base << n) else "." for n in range(4))


def main():
    path = sys.argv[1]
    raw = "--raw" in sys.argv
    ent = load(path)
    if not ent:
        print("empty dump")
        return
    f0 = ent[0][0]
    print("%d writes over frames %d..%d (%.1f s)" % (len(ent), f0, ent[-1][0], (ent[-1][0] - f0) / 50.0))
    sizes = Counter(e[3] for e in ent)
    print("access sizes:", dict(sizes))

    # per-channel register state, replayed in order
    st = [dict(lc=0, len=0, per=0, vol=0, on=False) for _ in range(4)]
    dmaen = True
    starts = [[] for _ in range(4)]
    volw = [Counter() for _ in range(4)]
    story = []
    for frame, vpos, hpos, reg, v in words(ent):
        t = "%7d %3d.%03d" % (frame - f0, vpos, hpos)
        if raw:
            print("%s  %-8s %04X" % (t, regname(reg), v))
        if 0x0A0 <= reg < 0x0E0:
            n, sub = (reg - 0x0A0) >> 4, reg & 0xE
            c = st[n]
            if sub == 0x0: c["lc"] = (c["lc"] & 0xFFFF) | ((v & 0x1FF) << 16)
            elif sub == 0x2: c["lc"] = (c["lc"] & 0xFFFF0000) | (v & 0xFFFE)
            elif sub == 0x4: c["len"] = v
            elif sub == 0x6: c["per"] = v
            elif sub == 0x8:
                vol = 64 if v & 0x40 else v & 0x3F
                volw[n][vol] += 1
                if c["on"] and vol != c["vol"]:
                    story.append((frame, n, "%s ch%d vol %2d -> %2d while playing" % (t, n, c["vol"], vol)))
                c["vol"] = vol
        elif reg == 0x096:
            setb = bool(v & 0x8000)
            if v & 0x0200:
                dmaen = setb
            for n in range(4):
                if not v & (1 << n):
                    continue
                c = st[n]
                if setb and not c["on"]:
                    c["on"] = True
                    starts[n].append((frame, c["lc"], c["len"], c["per"], c["vol"]))
                    story.append((frame, n, "%s ch%d START lc $%06X len %5d per %4d vol %2d" %
                                  (t, n, c["lc"], c["len"], c["per"], c["vol"])))
                elif not setb and c["on"]:
                    c["on"] = False
                    story.append((frame, n, "%s ch%d stop" % (t, n)))
        elif reg in (0x09A, 0x09C):
            story.append((frame, -1, "%s %s %s aud %s" % (t, regname(reg), "set" if v & 0x8000 else "clr",
                                                        bits(v, 0x80))))
    if raw:
        return
    print()
    for n in range(4):
        s = starts[n]
        print("ch%d: %d starts, volumes written %s" % (n, len(s), dict(sorted(volw[n].items()))))
        for (lc, ln, per), k in Counter((x[1], x[2], x[3]) for x in s).most_common(8):
            vols = Counter(x[4] for x in s if (x[1], x[2], x[3]) == (lc, ln, per))
            print("    $%06X len %5d per %4d  x%-4d vol at start %s" % (lc, ln, per, k, dict(vols)))
    print("\n--- the last 120 events ---")
    for _, _, line in story[-120:]:
        print(line)


if __name__ == "__main__":
    main()

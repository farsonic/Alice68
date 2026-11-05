#!/usr/bin/env python3
"""Build test SD card images for Alice68's hardfile support.

    python3 -I tools/hdf-test/mkimg.py OUTDIR

OUTDIR/card32.img: MBR, a FAT32 partition at sector 8192 (as on a Pi card)
holding hardfiles; OUTDIR/card16.img the same as FAT16. The hardfiles are
also left in OUTDIR so the tests can compare every block. Each data block
holds its own block number and a file tag, so a misplaced block shows.

Needs mkfs.fat, mtools and sfdisk.
SPDX-License-Identifier: GPL-2.0-only"""
import os
import struct
import subprocess
import sys

BS = 512


def csum(block, n):
    """set the RDB checksum (long 2) so the first n longs sum to 0"""
    longs = list(struct.unpack(">%dI" % n, block[:4 * n]))
    longs[2] = 0
    s = (-sum(longs)) & 0xffffffff
    block[8:12] = struct.pack(">I", s)


def rdb_hdf(path, tag, cyls, heads, secs, parts, rdb_at=0):
    """parts: (name, lowcyl, highcyl, bootable, bootpri, dostype)"""
    nblocks = cyls * heads * secs
    data = bytearray()
    for b in range(nblocks):
        data += (struct.pack(">I4s", b, tag) * (BS // 8))
    rdsk = bytearray(BS)
    rdsk[0:4] = b"RDSK"
    struct.pack_into(">I", rdsk, 4, 64)                       # SummedLongs
    struct.pack_into(">I", rdsk, 12, 7)                       # HostID
    struct.pack_into(">I", rdsk, 16, BS)                      # BlockBytes
    struct.pack_into(">I", rdsk, 24, 0xffffffff)              # BadBlockList
    struct.pack_into(">I", rdsk, 28, rdb_at + 1)              # PartitionList
    struct.pack_into(">I", rdsk, 32, 0xffffffff)              # FileSysHeaderList
    struct.pack_into(">I", rdsk, 36, 0xffffffff)              # DriveInit
    struct.pack_into(">III", rdsk, 64, cyls, secs, heads)
    csum(rdsk, 64)
    data[rdb_at * BS:(rdb_at + 1) * BS] = rdsk
    for i, (name, lo, hi, boot, pri, dostype) in enumerate(parts):
        p = bytearray(BS)
        p[0:4] = b"PART"
        struct.pack_into(">I", p, 4, 64)
        struct.pack_into(">I", p, 12, 7)
        nxt = rdb_at + 2 + i if i + 1 < len(parts) else 0xffffffff
        struct.pack_into(">I", p, 16, nxt)
        struct.pack_into(">I", p, 20, 1 if boot else 0)
        p[36] = len(name)
        p[37:37 + len(name)] = name.encode()
        env = [16, 128, 0, heads, 1, secs, 2, 0, 0, lo, hi, 30, 0, 0x1fe00, 0x7ffffffe,
               pri & 0xffffffff, dostype]
        struct.pack_into(">%dI" % len(env), p, 128, *env)
        csum(p, 64)
        data[(rdb_at + 1 + i) * BS:(rdb_at + 2 + i) * BS] = p
    open(path, "wb").write(data)


def raw_hdf(path, tag, nblocks):
    data = bytearray()
    for b in range(nblocks):
        data += struct.pack(">I4s", b, tag) * (BS // 8)
    data[0:4] = b"DOS\x01"
    open(path, "wb").write(data)


def run(*a):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def card(img, fat, out):
    part_start, part_mb = 8192, 96
    size = (part_start * BS) + part_mb * 1024 * 1024
    with open(img, "wb") as f:
        f.truncate(size)
    ptype = "c" if fat == 32 else "6"
    subprocess.run(["sfdisk", "-q", img], input=("label: dos\nstart=%d, type=%s\n" % (part_start, ptype)).encode(),
                   check=True, stdout=subprocess.DEVNULL)
    off = part_start * BS
    fs = out + "/fs.img"
    with open(fs, "wb") as f:
        f.truncate(part_mb * 1024 * 1024)
    if fat == 32:
        run("mkfs.fat", "-F", "32", "-s", "1", fs)              # 512-byte clusters: many pieces
    else:
        run("mkfs.fat", "-F", "16", "-s", "4", fs)
    m = "-i", fs
    run("mcopy", *m, out + "/kernel8.bin", "::kernel8.img")
    run("mcopy", *m, out + "/filler1.bin", "::filler1.bin")
    run("mcopy", *m, out + "/filler2.bin", "::filler2.bin")
    run("mdel", *m, "::filler1.bin")                          # a hole: games.hdf goes in pieces
    if fat == 32:                                             # forget FSInfo's next-free hint, so
        with open(fs, "r+b") as f:                            # the allocator starts at the hole
            f.seek(48)
            fsinfo = struct.unpack("<H", f.read(2))[0]
            f.seek(fsinfo * BS + 492)
            f.write(b"\xff\xff\xff\xff")
    run("mcopy", *m, out + "/games.hdf", "::games.hdf")
    run("mcopy", *m, out + "/Workbench 3.1.hdf", "::Workbench 3.1.hdf")
    for i in range(48):                                       # a comb of holes: frag.hdf in many pieces
        run("mcopy", *m, out + "/small.bin", "::s%02d.bin" % i)
    for i in range(0, 48, 2):
        run("mdel", *m, "::s%02d.bin" % i)
    if fat == 32:
        with open(fs, "r+b") as f:
            f.seek(48)
            fsinfo = struct.unpack("<H", f.read(2))[0]
            f.seek(fsinfo * BS + 492)
            f.write(b"\xff\xff\xff\xff")
    run("mcopy", *m, out + "/frag.hdf", "::frag.hdf")
    run("mcopy", *m, out + "/raw.hdf", "::raw.hdf")
    run("mmd", *m, "::HDF")
    run("mcopy", *m, out + "/extra.hdf", "::HDF/extra.hdf")
    with open(img, "r+b") as f, open(fs, "rb") as s:
        f.seek(off)
        f.write(s.read())
    os.remove(fs)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    open(out + "/kernel8.bin", "wb").write(os.urandom(300 * 1024))
    open(out + "/filler1.bin", "wb").write(b"\xaa" * 700 * 1024)
    open(out + "/filler2.bin", "wb").write(b"\x55" * 100 * 1024)
    open(out + "/small.bin", "wb").write(b"\x11" * 8 * 1024)
    ffs = 0x444f5303                                          # DOS\3
    rdb_hdf(out + "/Workbench 3.1.hdf", b"WBEN", 160, 2, 32, [("DH0", 2, 159, True, 5, ffs)])
    rdb_hdf(out + "/games.hdf", b"GAME", 128, 4, 32,
            [("DH1", 1, 63, False, 0, ffs), ("GAMES", 64, 127, True, -5, 0x50465303)], rdb_at=3)
    rdb_hdf(out + "/extra.hdf", b"XTRA", 64, 1, 32, [("DH0", 2, 63, True, 0, ffs)])
    raw_hdf(out + "/raw.hdf", b"RAWW", 2048)
    rdb_hdf(out + "/frag.hdf", b"FRAG", 64, 2, 32, [("FR0", 2, 63, False, 0, ffs)])
    card(out + "/card32.img", 32, out)
    card(out + "/card16.img", 16, out)
    print("ok:", ", ".join(sorted(f for f in os.listdir(out) if f.endswith((".img", ".hdf")))))


if __name__ == "__main__":
    main()

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


def test_hunk():
    """a load file for the filesystem loader: code (8 longs), data (4 longs),
    bss (16 longs); RELOC32 in the code to the data and to itself, RELOC32SHORT
    in the data to the bss"""
    L = lambda *v: struct.pack(">%dI" % len(v), *v)
    f = L(0x3f3, 0, 3, 0, 2, 8, 4, 16)
    f += L(0x3e9, 8, 4, 0x11111111, 16, 0x22222222, 0x4e754e75, 0, 0, 0)
    f += L(0x3ec, 1, 1, 0, 1, 0, 8, 0) + L(0x3f2)
    f += L(0x3ea, 4, 0xaaaa0000, 12, 0xbbbb0000, 0xcccc0000)
    f += struct.pack(">I4H", 0x3fc, 1, 2, 4, 0) + L(0x3f2)      # count 1, hunk 2, offset 4, end: 4 words, a long's worth
    f += L(0x3eb, 16) + L(0x3f2)
    return f


def root_block(data, blk, name):
    """an FFS root block: type 2 (T_SHORT), secondary type 1 (ST_ROOT), the volume name at 432"""
    o = blk * BS
    data[o:o + BS] = bytes(BS)
    struct.pack_into(">I", data, o, 2)
    struct.pack_into(">I", data, o + 508, 1)
    data[o + 432] = len(name)
    data[o + 433:o + 433 + len(name)] = name.encode()


def rdb_hdf(path, tag, cyls, heads, secs, parts, rdb_at=0, vols=None, fs=None):
    """parts: (name, lowcyl, highcyl, bootable, bootpri, dostype); vols: FFS volume
    names per partition (root blocks); fs: (dostype, version, patchflags, loadfile)"""
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
    fs_at = rdb_at + 1 + len(parts)
    struct.pack_into(">I", rdsk, 32, fs_at if fs else 0xffffffff)   # FileSysHeaderList
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
        if vols and vols[i]:
            n = (hi - lo + 1) * heads * secs
            root_block(data, lo * heads * secs + (2 + n - 1) // 2, vols[i])
    if fs:
        dostype, version, patchflags, load = fs
        lseg_at = fs_at + 1
        chunks = [load[k:k + 492] for k in range(0, len(load), 492)]
        h = bytearray(BS)
        h[0:4] = b"FSHD"
        struct.pack_into(">I", h, 4, 64)
        struct.pack_into(">I", h, 12, 7)
        struct.pack_into(">I", h, 16, 0xffffffff)
        struct.pack_into(">III", h, 32, dostype, version, patchflags)
        struct.pack_into(">I", h, 60, 4096)                    # dn_StackSize
        struct.pack_into(">i", h, 64, 10)                      # dn_Priority
        struct.pack_into(">i", h, 72, lseg_at)                 # dn_SegList: the first LSEG block
        struct.pack_into(">i", h, 76, -1)                      # dn_GlobalVec
        csum(h, 64)
        data[fs_at * BS:(fs_at + 1) * BS] = h
        for k, c in enumerate(chunks):
            g = bytearray(BS)
            g[0:4] = b"LSEG"
            struct.pack_into(">I", g, 4, 5 + (len(c) + 3) // 4)
            struct.pack_into(">I", g, 12, 7)
            struct.pack_into(">I", g, 16, lseg_at + k + 1 if k + 1 < len(chunks) else 0xffffffff)
            g[20:20 + len(c)] = c
            csum(g, 5 + (len(c) + 3) // 4)
            data[(lseg_at + k) * BS:(lseg_at + k + 1) * BS] = g
    open(path, "wb").write(data)


def raw_hdf(path, tag, nblocks):
    """a single-partition hardfile as WinUAE makes them: FFS from block 0,
    geometry 32 sectors, 1 surface, 2 reserved; the root block (type 2,
    secondary type 1) at (2 + last block) / 2"""
    data = bytearray()
    for b in range(nblocks):
        data += struct.pack(">I4s", b, tag) * (BS // 8)
    data[0:4] = b"DOS\x01"
    root_block(data, (2 + (nblocks // 32) * 32 - 1) // 2, "Raw")
    open(path, "wb").write(data)


def run(*a):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def card(img, fat, out):
    part_start, part_mb = 8192, 96
    amiga_start = part_start + part_mb * 2048                   # the SD card's own Amiga partition (0x76)
    amiga = open(out + "/sdpart.bin", "rb").read()
    size = amiga_start * BS + len(amiga)
    with open(img, "wb") as f:
        f.truncate(size)
    ptype = "c" if fat == 32 else "6"
    subprocess.run(["sfdisk", "-q", img], input=("label: dos\nstart=%d, size=%d, type=%s\nstart=%d, size=%d, type=76\n"
                          % (part_start, part_mb * 2048, ptype, amiga_start, len(amiga) // BS)).encode(),
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
    run("mcopy", *m, out + "/cmdline.txt", "::cmdline.txt")
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
        f.seek(amiga_start * BS)
        f.write(amiga)
    os.remove(fs)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    open(out + "/kernel8.bin", "wb").write(os.urandom(300 * 1024))
    open(out + "/filler1.bin", "wb").write(b"\xaa" * 700 * 1024)
    open(out + "/filler2.bin", "wb").write(b"\x55" * 100 * 1024)
    open(out + "/small.bin", "wb").write(b"\x11" * 8 * 1024)
    ffs = 0x444f5303                                          # DOS\3
    rdb_hdf(out + "/Workbench 3.1.hdf", b"WBEN", 160, 2, 32, [("DH0", 2, 159, True, 5, ffs)], vols=["Workbench"])
    rdb_hdf(out + "/games.hdf", b"GAME", 128, 4, 32,
            [("DH1", 1, 63, False, 0, ffs), ("GAMES", 64, 127, True, -5, 0x50465303)], rdb_at=3,
            vols=["Games1", None], fs=(0x50465303, 0x00120000, 0x180, test_hunk()))
    # extra.hdf's volume has the name of the SD card partition's: a duplicate
    rdb_hdf(out + "/extra.hdf", b"XTRA", 64, 1, 32, [("DH5", 2, 63, True, 0, ffs)], vols=["SDWork"])
    open(out + "/testhunk.bin", "wb").write(test_hunk())
    rdb_hdf(out + "/sdpart.bin", b"SDPT", 128, 1, 64, [("DH0", 2, 127, True, 0, ffs)], vols=["SDWork"])
    raw_hdf(out + "/raw.hdf", b"RAWW", 2048)
    rdb_hdf(out + "/frag.hdf", b"FRAG", 64, 2, 32, [("FR0", 2, 63, False, 0, ffs)], vols=["Frag"])
    open(out + "/cmdline.txt", "w").write("aga.look=0 aga.scale=1\n")
    card(out + "/card32.img", 32, out)
    card(out + "/card16.img", 16, out)
    # the same card with other options in cmdline.txt
    for name, line in (("boot", "aga.look=0 hdf.boot=workbench_3.1.hdf aga.scale=1"),
                       ("raw", "hdf.boot=RAW"), ("off", "hdf=off"), ("ro", "hdf.readonly aga.look=0"),
                       ("noboot", "hdf.noboot"), ("bootnb", "hdf.noboot hdf.boot=raw.hdf"),
                       ("extra", "hdf.boot=extra")):
        img = "%s/card32_%s.img" % (out, name)
        with open(out + "/card32.img", "rb") as a, open(img, "wb") as b:
            b.write(a.read())
        open(out + "/cmdline.txt", "w").write(line + "\n")
        run("mcopy", "-o", "-i", "%s@@%d" % (img, 8192 * BS), out + "/cmdline.txt", "::cmdline.txt")
    print("ok:", ", ".join(sorted(f for f in os.listdir(out) if f.endswith((".img", ".hdf")))))


if __name__ == "__main__":
    main()

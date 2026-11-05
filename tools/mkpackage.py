"""Assemble the AGA-PISTORM release package.

    python tools/mkpackage.py [DEST]     default: the Desktop\\AGA-Pistorm folder

One package for every board: a kernel for the classic PiStorm and one for
the PiStorm16, and an installer that picks the right one. Give DEST
explicitly to build elsewhere - the default is the author's published copy.

Everything comes from the repo and the current build; the checks at the end
refuse to leave a package with CRLF in an Amiga script, a missing file, or
the author's real name in it. The complete source of the kernels' Emu68 part
goes next to the package folder, made once for each Emu68 commit.
SPDX-License-Identifier: GPL-2.0-only
"""
import gzip
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(ROOT, "package")
DEST = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.expanduser("~"), "Desktop", "AGA-Pistorm")
VERSION = "1.120"
MARK = ".aga-pistorm-package"
# Every place the release carries its version, and how to read it there. The
# installer said 1.101 from 1.101 to 1.108 because nothing bloody looked: now
# a package whose stamps do not all say VERSION is refused before it is built.
STAMPS = [
    ("package/amiga/Install", r"\$VER: Install AGA-PISTORM (\S+) "),
    ("package/amiga/Install", r'\(set #ver "([^"]+)"\)'),
    ("package/amiga/Uninstall", r"\$VER: Uninstall AGA-PISTORM (\S+) "),
    ("tools/agaboot/agaboot.c", r'#define AGA_PISTORM_VERSION "([^"]+)"'),
    ("package/README.txt", r"\AAGA-PISTORM (\S+) "),
    ("package/README.txt", r"WHAT'S NEW IN (\S+)"),
    ("README.md", r"\*\*Release (\S+) \("),
    ("arm/aga_diag.c", r'#define AGA_KERNEL_VERSION "([^"]+)"'),                  # the kernel's own (agaboot's stale-kernel warning)
    ("third_party/Emu68/include/logo/logo_aga_rgb.h", r"Version (\S+), \d+x\d+"),   # the boot splash
    ("arm/emu68-aga.patch", r"Version (\S+), \d+x\d+"),                            # ...as the kernels get it
]
THIRD_PARTY_LICENCES = {os.path.join("Licenses", f) for f in ("Capstone-LICENSE.txt", "Capstone-LICENSE_LLVM.txt",
                                                              "libdeflate-COPYING.txt", "Emu68-third-party-notices.txt")}
WSL_GPL2 = r"\\wsl.localhost\Ubuntu-24.04\usr\share\common-licenses\GPL-2"
EMU68 = os.path.join(ROOT, "third_party", "Emu68")
EMU68_REV = "0ba3a89"
EMU68_SUBMODULES = ("external/capstone", "external/libdeflate")
# The Emu68 part's complete source: made and published once for each
# EMU68_REV, and not rebuilt for the fun of it. 0ba3a89's first went out with
# 1.106; later packages point there.
EMU68_SRC_VERSION = "1.106"
EMU68_SRC = "AGA-PISTORM-%s-emu68-src.tar.gz" % EMU68_SRC_VERSION
# VideoCore.card's upstream at the commit the AGA-PISTORM diff
# (package/videocore-src) is against, with its submodules: the rest of what
# building C:...VideoCore.card needs. From the fork the changes were made in.
VIDEOCORE = os.path.join(os.path.dirname(ROOT), "PI Mailbox GPU driver", "VideoCore.card-fork")
VIDEOCORE_REV = "e49193a"
VIDEOCORE_SUBMODULES = ("devicetree.resource", "unicam.resource (with its own: devicetree.resource, mailbox.resource)")
VIDEOCORE_SRC = "VideoCore.card-%s-src.tar.gz" % VIDEOCORE_REV

# Someone with only the package's Source/ folder (and the compilers named in
# package/HOW-TO-BUILD.txt) must be able to build everything the package ships.
# The Amiga tools go into the source zip whole - every file git has for them
# except test rigs and the programs their build.sh makes - and I refuse a
# package where a build.sh compiles a file the zip lacks, or where a program,
# driver or overlay has no source named here. A list of files by hand left
# whdwrap.c, C:WHDLoad-AGA's source, out of 1.116 and 1.117's first package -
# never again.
AMIGA_TOOL_DIRS = ("tools/agaboot", "tools/agastat", "tools/ahi", "tools/mkicon")
SOURCE_OF = {          # package file (under Amiga/AGA-Pistorm) -> its source in Source/
    "Files/C/agaboot": "AGA-PISTORM/tools/agaboot/agaboot.c",
    "Files/C/agastat": "AGA-PISTORM/tools/agastat/agastat.c",
    "Files/C/WHDLoad-AGA": "AGA-PISTORM/tools/agaboot/whdwrap.c",
    "Files/Utilities/AGA-SoundMonitor": "AGA-PISTORM/tools/agaboot/smon.c",
    "Files/Libs/Picasso96/VideoCore.card": "VideoCore.card/VideoCore-AGA-PISTORM.diff",
}

# The notices of code Emu68 compiles into the kernels whose licences demand
# they travel with the binary: (repository, path, first line, last line, what)
NOTICES = [
    ("Emu68", "src/pistorm/ps_classic_protocol.c", 1, 7, "the classic PiStorm bus protocol (MIT; classic kernels)"),
    ("Emu68", "src/pistorm/ps_protocol.c", 1, 7, "the PiStorm16 bus protocol (MIT; PiStorm16 kernel)"),
    ("Emu68", "src/pistorm/libxsvf/COPYING", 1, 18, "libxsvf, the CPLD programmer (ISC; classic kernels)"),
    ("Emu68", "src/ppc/kernel.c", 1, 21, "the PowerPC ROM (MIT)"),
    ("Emu68", "src/support.c", 850, 878, "qsort (NetBSD, BSD-3-Clause)"),
    ("Emu68", "src/math/exp2.c", 1, 26, "exp2 (FreeBSD, BSD-2-Clause)"),
    ("Emu68", "src/math/__cos.c", 1, 11, "the msun maths functions (Sun Microsystems)"),
    ("Emu68", "src/tlsf.c", 1, 4, "TLSF allocator (AROS)"),
    ("Emu68", "src/raspi/topaz.c", 1, 4, "the topaz font (AROS)"),
    ("Emu68", "include/lists.h", 1, 9, "exec lists (AROS)"),
    ("Emu68", "include/nodes.h", 1, 4, "exec nodes (AROS)"),
    ("external/capstone", "arch/M68K/M68KDisassembler.c", 1, 28, "Capstone's M68K disassembler, from Musashi (MIT)"),
]
MUSL = (
    "The maths library (src/math) is from musl, MIT licence:\n\n"
    "Copyright (c) 2005-2020 Rich Felker, et al.\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining\n"
    "a copy of this software and associated documentation files (the\n"
    "\"Software\"), to deal in the Software without restriction, including\n"
    "without limitation the rights to use, copy, modify, merge, publish,\n"
    "distribute, sublicense, and/or sell copies of the Software, and to\n"
    "permit persons to whom the Software is furnished to do so, subject to\n"
    "the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be\n"
    "included in all copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n"
    "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF\n"
    "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.\n"
    "IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY\n"
    "CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,\n"
    "TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE\n"
    "SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n\n"
    "The PiStorm protocol files and the PowerPC ROM above say MIT; the MIT\n"
    "licence text is the one quoted here.\n")

# Emu68 also carries binary parts built elsewhere, as data it hands over at
# start - not built from its source, their origin not recorded in it, the sod
BLOBS = (
    "Also inside the kernels: parts Emu68 carries as data (src/boards/*.h and\n"
    "src/pistorm/efinix_firmware_*.h at git %s), built elsewhere and handed\n"
    "over at start - separate programs, not built from Emu68's source:\n\n"
    "  The Emu68 ROM and the Amiga modules in it - devicetree.resource,\n"
    "  unicam.resource, mailbox.resource, gic400.library, brcm-sdhc.device,\n"
    "  brcm-emmc.device, 68040.library, powerpc.library - by Michal Schulz,\n"
    "  whose sources are at https://github.com/michalsc (Emu68-tools and its\n"
    "  repositories). In every kernel. Emu68's source does not record which\n"
    "  versions.\n\n"
    "  The PiStorm32-lite's and PiStorm16's FPGA firmware (Efinix\n"
    "  bitstreams), loaded into the PiStorm at start. In the PiStorm16 kernel\n"
    "  only. Emu68's source does not record where they are built from.\n" % EMU68_REV)


def git(repo, *args):
    return subprocess.run(["git", "-C", repo] + list(args), check=True, capture_output=True).stdout


def sub_commit(path):
    """the commit EMU68_REV records for a submodule"""
    return git(EMU68, "rev-parse", "%s:%s" % (EMU68_REV, path)).decode().strip()


def notices():
    out = ["Notices of code built into the AGA-PISTORM kernels (Emu68 %s)" % EMU68_REV,
           "=" * 60, "",
           "The AGA-PISTORM kernels are Emu68 1.1.0-beta.1 (MPL-2.0, Licenses/MPL-2.0.txt)",
           "with AGA-PISTORM's chipset added (GPL-2.0); as a whole they are distributed",
           "under the GPL-2.0 (Licenses/GPL-2.0.txt), as MPL-2.0 section 3.3 allows.",
           "Emu68 compiles in the code below; its licences ask for these notices to go",
           "with the binary.", ""]
    for repo, path, a, b, what in NOTICES:
        if repo == "Emu68":
            text = git(EMU68, "show", "%s:%s" % (EMU68_REV, path))
        else:
            text = git(os.path.join(EMU68, repo), "show", "%s:%s" % (sub_commit(repo), path))
        # Emu68's copies of the AROS files already carry U+FFFD where their
        # copyright sign was: write the bloody thing as (C)
        lines = text.decode("utf-8", "replace").replace("Copyright �", "Copyright (C)").splitlines()[a - 1:b]
        out += ["-" * 60, "%s - %s/%s" % (what, "Emu68" if repo == "Emu68" else "Emu68/" + repo, path), ""]
        out += lines + [""]
    out += ["-" * 60, MUSL, "-" * 60, BLOBS]
    return "\n".join(out) + "\n"


def git_tarball(dest, parts, note_name, note):
    """(repo, rev, prefix) parts, each as committed, plus a note, as one
    reproducible .tar.gz"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as out:
        for repo, rev, prefix in parts:
            # the files as committed: with core.autocrlf=true (Git for Windows'
            # default) git archive writes CRLF, and then an LF patch will not
            # apply to the tree. Git's default, for fuck's sake.
            data = subprocess.run(["git", "-c", "core.autocrlf=false", "-C", repo, "archive", "--format=tar", rev],
                                  check=True, capture_output=True).stdout
            with tarfile.open(fileobj=io.BytesIO(data)) as t:
                for m in t.getmembers():
                    f = t.extractfile(m) if m.isfile() else None
                    m.name = prefix + m.name
                    if m.islnk():
                        m.linkname = prefix + m.linkname
                    out.addfile(m, f)
        ti = tarfile.TarInfo(note_name)
        ti.size = len(note)
        ti.mtime = 0
        out.addfile(ti, io.BytesIO(note))
    with open(dest, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0, compresslevel=9) as z:
            z.write(buf.getvalue())


def videocore_source(dest):
    """VideoCore.card at VIDEOCORE_REV with devicetree.resource and
    unicam.resource at the commits it records - the upstream half of the
    shipped VideoCore.card; the AGA-PISTORM changes are the diff in the
    source zip"""
    if not os.path.isdir(VIDEOCORE):
        sys.exit("PACKAGE REFUSED - the VideoCore.card repository is not at %s" % VIDEOCORE)
    parts = []

    def add(repo, rev, prefix):
        # with the submodules it records, theirs too: unicam.resource has its
        # own (mailbox.resource), and without it VideoCore.card's cmake stops
        # dead, of course.
        parts.append((repo, rev, prefix))
        try:
            mods = git(repo, "show", "%s:.gitmodules" % rev).decode()
        except subprocess.CalledProcessError:
            return
        for path in re.findall(r"^\s*path\s*=\s*(\S+)", mods, re.M):
            sub = os.path.join(repo, path)
            if not os.path.isdir(os.path.join(sub, ".git")) and not os.path.isfile(os.path.join(sub, ".git")):
                sys.exit("PACKAGE REFUSED - submodule %s is not checked out" % sub)
            add(sub, git(repo, "rev-parse", "%s:%s" % (rev, path)).decode().strip(), prefix + path + "/")
    add(VIDEOCORE, VIDEOCORE_REV, "VideoCore.card/")
    note = ("VideoCore.card git %s (https://github.com/michalsc/VideoCore.card, MPL-2.0)\n"
            "with its submodules %s at the commits it records. AGA-PISTORM ships it\n"
            "with changes: apply VideoCore-AGA-PISTORM.diff from the source zip's\n"
            "VideoCore.card folder and copy overlay.c and version_gen.h.in as its\n"
            "README.txt says (see HOW-TO-BUILD.txt).\n"
            % (VIDEOCORE_REV, ", ".join(VIDEOCORE_SUBMODULES))).encode()
    git_tarball(dest, parts, "VideoCore.card/AGA-PISTORM-SOURCE.txt", note)


def emu68_source(dest):
    """Emu68 at EMU68_REV with capstone and libdeflate at the commits it
    records, as one reproducible .tar.gz: the complete source of the kernels'
    Emu68 part, in the package's Source/ (GPL-2.0 section 3). I make it once
    per EMU68_REV (the bytes do not change), so 0ba3a89's is still 1.106's."""
    parts = [(EMU68, EMU68_REV, "Emu68/")] + [(os.path.join(EMU68, s), sub_commit(s), "Emu68/%s/" % s)
                                               for s in EMU68_SUBMODULES]
    note = ("This is the complete source of the Emu68 part of the AGA-PISTORM kernels\n"
            "(from AGA-PISTORM %s on): Emu68 git %s (https://github.com/michalsc/Emu68,\n"
            "MPL-2.0) with its submodules %s at the commits it records.\n"
            "AGA-PISTORM's own part (GPL-2.0) and arm/emu68-aga.patch are in the\n"
            "package's Source/AGA-PISTORM-<version>-src.zip. To build: apply the patch to\n"
            "this Emu68 folder (git apply), put it at third_party/Emu68 of the\n"
            "AGA-PISTORM source and run tools/build-wsl.sh (see EMU68-SOURCE.txt there).\n"
            % (EMU68_SRC_VERSION, EMU68_REV, ", ".join(EMU68_SUBMODULES))).encode()
    git_tarball(dest, parts, "Emu68/AGA-PISTORM-SOURCE.txt", note)


def stamp_problems():
    """Each STAMPS place that does not say VERSION, and a built agaboot that
    does not carry it either - built before the version changed, the lazy sod."""
    out = []
    for rel, pat in STAMPS:
        t = open(os.path.join(ROOT, rel), encoding="latin-1").read()
        m = re.search(pat, t)
        if not m:
            out.append("%s: no version found" % rel)
        elif m.group(1) != VERSION:
            out.append("%s says %s" % (rel, m.group(1)))
    if VERSION.encode() not in open(os.path.join(ROOT, "tools", "agaboot", "agaboot.new"), "rb").read():
        out.append("tools/agaboot/agaboot.new is not built with %s" % VERSION)
    return out


def text(src, dst, crlf):
    data = open(src, "rb").read().replace(b"\r\n", b"\n")
    if crlf:
        data = data.replace(b"\n", b"\r\n")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(data)


def binary(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


# The entries this script owns in DEST. Anything else there - screenshots, a
# demo video - belongs to the author, and I do not touch it.
MANAGED = ["README.txt", "PiStorm16.txt", "SD-Setup.cmd", "PC", "Amiga", "Licenses", "Source", "CHECKSUMS.txt", MARK]


def main():
    bad = stamp_problems()
    if bad:
        sys.exit("PACKAGE REFUSED - not everything says %s:\n  %s" % (VERSION, "\n  ".join(bad)))
    os.makedirs(DEST, exist_ok=True)
    present = [e for e in MANAGED if os.path.exists(os.path.join(DEST, e))]
    if present and not os.path.exists(os.path.join(DEST, MARK)):
        sys.exit("%s already has %s, not made by this script - not touching it" % (DEST, ", ".join(present)))
    for e in present:
        p = os.path.join(DEST, e)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    open(os.path.join(DEST, MARK), "w").write("made by tools/mkpackage.py\n")
    if os.name == "nt":                     # bookkeeping, not for the reader - hide the bugger
        os.system('attrib +h "%s" >NUL' % os.path.join(DEST, MARK))

    A = os.path.join(DEST, "Amiga", "AGA-Pistorm")
    # the PC side: CRLF, because Notepad and cmd.exe are the sorry sods who read them
    text(os.path.join(PKG, "README.txt"), os.path.join(DEST, "README.txt"), True)
    text(os.path.join(PKG, "PiStorm16.txt"), os.path.join(DEST, "PiStorm16.txt"), True)
    text(os.path.join(PKG, "SD-Setup.cmd"), os.path.join(DEST, "SD-Setup.cmd"), True)
    text(os.path.join(PKG, "pc", "SD-Setup.ps1"), os.path.join(DEST, "PC", "SD-Setup.ps1"), True)
    # the Amiga side: LF only - CRLF in an AmigaDOS or Installer script breaks it,
    # and then you get to debug that shit
    text(os.path.join(PKG, "amiga", "Install"), os.path.join(A, "Install"), False)
    text(os.path.join(PKG, "amiga", "Uninstall"), os.path.join(A, "Uninstall"), False)
    text(os.path.join(PKG, "README.txt"), os.path.join(A, "ReadMe.txt"), False)
    # the Chipset menu as a drawer, for an AmigaOS without ToolsDaemon or DOpus:
    # one command each, run through IconX (the installer makes the icons, the
    # lucky sods)
    for s in sorted(os.listdir(os.path.join(PKG, "amiga", "Chipset"))):
        text(os.path.join(PKG, "amiga", "Chipset", s), os.path.join(A, "Files", "Chipset", s), False)
    for icon, where in (("Install.info", A), ("Uninstall.info", A), ("ReadMe.txt.info", A),
                        ("AGA-Pistorm.info", os.path.join(DEST, "Amiga"))):
        binary(os.path.join(PKG, "icons", icon), os.path.join(where, icon))
    F = os.path.join(A, "Files")
    binary(os.path.join(ROOT, "tools", "agaboot", "agaboot.new"), os.path.join(F, "C", "agaboot"))
    binary(os.path.join(ROOT, "tools", "agastat", "agastat.new"), os.path.join(F, "C", "agastat"))
    # the WHDLoad stand-in, put in C:WHDLoad's place by the AmiKit choice
    # (agaboot WHDWRAP; tools/agaboot/whdwrap.c)
    binary(os.path.join(ROOT, "tools", "agaboot", "whdwrap.new"), os.path.join(F, "C", "WHDLoad-AGA"))
    # the Sound monitor as a program of its own, with an icon in SYS:Utilities
    binary(os.path.join(ROOT, "tools", "agaboot", "smon.new"), os.path.join(F, "Utilities", "AGA-SoundMonitor"))
    binary(os.path.join(ROOT, "dist", "wsl", "Emu68-pistorm-classic-aga-wsl.gz"),
           os.path.join(F, "Kernel", "Emu68-classic-aga.gz"))
    # the same for a Pi 3 (tools/build-wsl.sh, -DAGA_PI3=ON); the installer
    # copies whichever matches the Pi under the name above
    binary(os.path.join(ROOT, "dist", "wsl", "Emu68-pistorm-classic-aga-pi3-wsl.gz"),
           os.path.join(F, "Kernel", "Emu68-classic-aga-pi3.gz"))
    # the PiStorm16 one: Emu68's "pistorm" variant, tools/build-wsl.sh ps16
    binary(os.path.join(ROOT, "dist", "ps16", "Emu68-pistorm16-aga.gz"),
           os.path.join(F, "Kernel", "Emu68-pistorm16-aga.gz"))
    binary(os.path.join(PKG, "files", "VideoCore.card"), os.path.join(F, "Libs", "Picasso96", "VideoCore.card"))
    for o in sorted(os.listdir(os.path.join(PKG, "files", "overlays"))):
        binary(os.path.join(PKG, "files", "overlays", o), os.path.join(F, "Overlays", o))

    # licences
    L = os.path.join(DEST, "Licenses")
    text(WSL_GPL2, os.path.join(L, "GPL-2.0.txt"), True)
    text(os.path.join(ROOT, "third_party", "Emu68", "LICENSE"), os.path.join(L, "MPL-2.0.txt"), True)
    # the libraries Emu68 links into the kernel: their licences ask for the
    # notices to travel with the binary, so they go in
    X = os.path.join(ROOT, "third_party", "Emu68", "external")
    text(os.path.join(X, "capstone", "LICENSE.TXT"), os.path.join(L, "Capstone-LICENSE.txt"), True)
    text(os.path.join(X, "capstone", "LICENSE_LLVM.TXT"), os.path.join(L, "Capstone-LICENSE_LLVM.txt"), True)
    text(os.path.join(X, "libdeflate", "COPYING"), os.path.join(L, "libdeflate-COPYING.txt"), True)
    # the rest of the code Emu68 compiles in, and the parts it carries as data
    n = os.path.join(L, "Emu68-third-party-notices.txt")
    open(n, "wb").write(notices().replace("\n", "\r\n").encode("utf-8"))
    # the chipset core follows UAE/WinUAE: their authors' notices
    text(os.path.join(PKG, "UAE-WinUAE-notice.txt"), os.path.join(L, "UAE-WinUAE-notice.txt"), True)

    # Source/: everything needed to build what the package ships - AGA-PISTORM's
    # own (no .git: commit authors; no test rigs), Emu68's and VideoCore.card's
    # upstream source, and how to build it all
    S = os.path.join(DEST, "Source")
    os.makedirs(S)
    zpath = os.path.join(S, "AGA-PISTORM-%s-src.zip" % VERSION)
    keep_dirs = ["core", "arm", "package"]
    keep_files = ["LICENSE", "COPYING", "MPL-2.0.txt", "README.md", "tools/build-wsl.sh", "tools/mklogo.py",
                  "tools/mklogo_rgb.py", "tools/audw.py", "tools/mkblep.py", "tools/mkpackage.py"]
    # the CRT look's GPU programs (arm/aga_v3d_code.h, 1.119): their maker and
    # py-videocore6's assembler (MIT, its notice in each file)
    keep_files += [f for f in git(ROOT, "ls-files", "-z", "tools/v3d").decode().split("\0") if f.endswith(".py")]
    for d in AMIGA_TOOL_DIRS:
        for f in git(ROOT, "ls-files", "-z", d).decode().split("\0"):
            name = f.rsplit("/", 1)[-1]
            if f and "/tests/" not in f and "." in name and not name.endswith(".new"):
                keep_files.append(f)              # not the test rigs nor agaboot, agaboot.new ...
    upstream = os.path.dirname(os.path.abspath(DEST))     # made once, kept next to the packages
    for name, make in ((EMU68_SRC, emu68_source), (VIDEOCORE_SRC, videocore_source)):
        if not os.path.exists(os.path.join(upstream, name)):
            make(os.path.join(upstream, name))
        shutil.copyfile(os.path.join(upstream, name), os.path.join(S, name))
    text(os.path.join(PKG, "HOW-TO-BUILD.txt"), os.path.join(S, "HOW-TO-BUILD.txt"), True)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for d in keep_dirs:
            for root, _, files in os.walk(os.path.join(ROOT, d)):
                rel = os.path.relpath(root, ROOT).replace("\\", "/")
                if rel.startswith(("package/files", "package/videocore-src")):
                    continue                  # what the build makes, or zipped on their own below
                                              # (the icons stay: the .info files are their only form)
                for f in files:
                    p = os.path.join(root, f)
                    z.write(p, os.path.join("AGA-PISTORM", os.path.relpath(p, ROOT)))
        for f in keep_files:
            z.write(os.path.join(ROOT, f), os.path.join("AGA-PISTORM", f))
        for f in sorted(os.listdir(os.path.join(PKG, "videocore-src"))):
            z.write(os.path.join(PKG, "videocore-src", f), os.path.join("VideoCore.card", f))
        z.writestr("AGA-PISTORM/EMU68-SOURCE.txt",
                   "The AGA-PISTORM kernels are Emu68 1.1.0-beta.1 (https://github.com/michalsc/Emu68,\n"
                   "git 0ba3a899341dee958332a4182911861420b03bff, MPL-2.0) with arm/emu68-aga.patch\n"
                   "applied and AGA-PISTORM's core/ and arm/ added. Emu68's submodules, at the\n"
                   "commits it records: external/capstone %s, external/libdeflate %s.\n\n"
                   "The Emu68 source: %s in the package's Source folder\n"
                   "(made with AGA-PISTORM %s, unchanged since), or:\n"
                   "  git clone https://github.com/michalsc/Emu68 third_party/Emu68\n"
                   "  git -C third_party/Emu68 checkout 0ba3a89\n"
                   "  git -C third_party/Emu68 submodule update --init\n"
                   "  git -C third_party/Emu68 apply ../../arm/emu68-aga.patch\n\n"
                   "Build: tools/build-wsl.sh (Ubuntu 24.04; its header lists the packages).\n"
                   "The classic kernels need the Arm GNU Toolchain 15.3.rel2 for big-endian\n"
                   "AArch64 (aarch64_be-none-linux-gnu) - gcc-14 miscompiles their vector code;\n"
                   "the Pi 3 kernel is the same with -DAGA_PI3=ON (build-wsl.sh makes both);\n"
                   "the PiStorm16 kernel: tools/build-wsl.sh ps16, with aarch64-linux-gnu-gcc-14.\n"
                   "The Amiga tools: tools/agaboot, agastat, ahi and mkicon each have a build.sh\n"
                   "(m68k-amigaos-gcc from amiga-gcc, in /opt/amiga).\n"
                   "The boot splash in the patch (include/logo/logo_aga_rgb.h) is made by\n"
                   "tools/mklogo_rgb.py from package/art/splash-src.webp.\n"
                   "The CRT look's GPU programs, arm/aga_v3d_code.h, are made by\n"
                   "tools/v3d/mkcrt.py (Python 3.11 or later) with py-videocore6's assembler\n"
                   "(Idein Inc., MIT, in tools/v3d/_videocore6).\n"
                   % (sub_commit("external/capstone"), sub_commit("external/libdeflate"),
                      EMU68_SRC, EMU68_SRC_VERSION))

    # ---- checks -----------------------------------------------------------------
    problems = []
    for s in ["Install", "Uninstall", "ReadMe.txt"] + ["Files/Chipset/" + c for c in os.listdir(os.path.join(A, "Files", "Chipset"))]:
        if b"\r" in open(os.path.join(A, s), "rb").read():
            problems.append("CR in " + s)
    # an Amiga tool's own headers (#include "x", no -I in its build.sh) must be
    # in the source zip too - 1.115's lacked agaboot's menu.h, for fuck's sake
    with zipfile.ZipFile(zpath) as z:
        names = set(z.namelist())
        for n in sorted(names):
            if n.startswith("AGA-PISTORM/tools/") and n.endswith((".c", ".h")):
                for inc in re.findall(rb'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', z.read(n), re.M):
                    if os.path.normpath(os.path.join(os.path.dirname(n), inc.decode())).replace("\\", "/") not in names:
                        problems.append("%s includes %s, not in the source zip" % (n[len("AGA-PISTORM/"):], inc.decode()))
        # ...and every file a tool's build.sh compiles (nothing includes
        # whdwrap.c: build.sh names the bugger, and that is all)
        for n in sorted(names):
            if n.startswith("AGA-PISTORM/tools/") and n.endswith("/build.sh"):
                for src in sorted(set(re.findall(rb'(?<![\w./-])([\w-]+\.(?:c|s|S|asm))(?!\w)', z.read(n)))):
                    if n.rsplit("/", 1)[0] + "/" + src.decode() not in names:
                        problems.append("%s compiles %s, not in the source zip" % (n[len("AGA-PISTORM/"):], src.decode()))
    # every program, driver and overlay the package ships has its source in
    # Source/: in the zip, or Emu68's own in the Emu68 tarball
    with tarfile.open(os.path.join(S, EMU68_SRC)) as t:
        emu68_names = set(t.getnames())
    for root, _, files in os.walk(A):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), A).replace("\\", "/")
            if f.endswith(".info") or b"\0" not in open(os.path.join(root, f), "rb").read():
                continue                          # icons and text files: their own source
            src = SOURCE_OF.get(rel)
            if rel.startswith("Files/Kernel/"):
                src = "AGA-PISTORM/arm/emu68-aga.patch"
            elif rel.startswith("Files/Overlays/") and f.endswith(".dtbo"):
                dts = f[:-len(".dtbo")] + ".dts"
                src = ("AGA-PISTORM/package/overlays-src/" + dts if os.path.exists(os.path.join(PKG, "overlays-src", dts))
                       else "Emu68/src/overlays/" + dts)
            if not src:
                problems.append("%s ships with no source named for it (SOURCE_OF in mkpackage.py)" % rel)
            elif src not in (emu68_names if src.startswith("Emu68/") else names):
                problems.append("%s: its source %s is not in Source/" % (rel, src))
    # What must never ship lives OUTSIDE the repo, one regex per line - spelling
    # it out here would put the fucker straight into the source archive this builds.
    private = os.path.join(os.path.expanduser("~"), ".aga-pistorm-private")
    pats = [l.strip() for l in open(private, encoding="utf-8") if l.strip()] if os.path.exists(private) else []
    if not pats:
        print("WARNING: no %s - the package was NOT checked for personal data" % private)
    secret = re.compile(("(?i)" + "|".join(pats)).encode() if pats else rb"(?!)")
    for root, _, files in os.walk(DEST):
        for f in files:
            p = os.path.join(root, f)
            data = open(p, "rb").read()
            if p.endswith(".zip"):
                with zipfile.ZipFile(p) as z:
                    data = b"".join(z.read(n) for n in z.namelist())
            # third-party licence texts ship verbatim and name their own authors
            # (Capstone's has a gmail address), which must not trip the check
            if os.path.relpath(p, DEST) in THIRD_PARTY_LICENCES:
                continue
            if secret.search(data):
                problems.append("personal data in " + os.path.relpath(p, DEST))
    if problems:
        sys.exit("PACKAGE REJECTED: " + "; ".join(problems))

    # checksums, so a copy can be verified - assuming anyone bothers
    lines = []
    for root, _, files in os.walk(DEST):
        for f in sorted(files):
            p = os.path.join(root, f)
            if f in (MARK, "CHECKSUMS.txt"):
                continue
            lines.append("%s  %s" % (hashlib.md5(open(p, "rb").read()).hexdigest(), os.path.relpath(p, DEST)))
    open(os.path.join(DEST, "CHECKSUMS.txt"), "w", newline="\r\n").write("\n".join(sorted(lines, key=lambda s: s[34:])) + "\n")

    total = 0
    for root, _, files in os.walk(DEST):
        for f in sorted(files):
            p = os.path.join(root, f)
            total += os.path.getsize(p)
            print("  %-58s %8d" % (os.path.relpath(p, DEST), os.path.getsize(p)))
    print("package: %s (%d files, %.1f KB)" % (DEST, sum(len(f) for _, _, f in os.walk(DEST)), total / 1024))
    print("Source/: the source zip, %s and %s - everything the package is built from" % (EMU68_SRC, VIDEOCORE_SRC))


if __name__ == "__main__":
    main()

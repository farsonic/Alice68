# Where Alice68's code comes from

Alice68's history starts with three commits that keep the inputs apart:

1. **AGA-PiStorm 1.120, as released.** `AGA-PISTORM-1.120-src.zip`
   (SHA-256 `a0a8dc01e0247ad504c2af5869e3fde392de990ef3252646bfc1db3830eddc38`,
   from `AGA-Pistorm_universal_v1.120.7z`), applied to Emu68 at upstream
   commit `0ba3a899341dee958332a4182911861420b03bff`
   (https://github.com/michalsc/Emu68). Emu68's submodules are kept as
   submodules at the commits that Emu68 commit records: capstone `accf4df`,
   libdeflate `bd925ae` and tiny-stl `6772983`.
2. **The EMU68AGA Bare Metal layer** (Future Retro Fusion, v0.10.44 kernel delta,
   from https://github.com/futureretrofusion/EMU68AGA-Bare-Metal), merged onto
   1.120. The Emu68 delta is a three-way merge, with the conflicts in `vectors.c`,
   `mmu.c` and `CMakeLists.txt` resolved by hand. FRF's `FRF_VIRTUAL_AGA` hooks
   were re-applied to 1.120's `arm/` and `core/` files. USPi (GPL-3.0-or-later)
   is left out; see THIRD_PARTY_NOTICES.md.
3. **Alice68:**
   - the Pi 4 port: DMA pool, Zorro III guard, serial log on GPIO 14, the
     on-screen log;
   - the xHCI driver, the Neptune pass-through and the virtual eject;
   - the splash;
   - the 68k driver and log tool;
   - `alice68hdf.device` (`amiga/hdf/`): hardfiles, the SD-card folders (its own FAT
     engine) and CD images (`iso9660.c`, written from ECMA-119, the Joliet
     specification and IEEE P1282 Rock Ridge; no code from other ISO readers) and the
     exFAT partition (`exfat.c`, written from Microsoft's published exFAT specification;
     no code from other exFAT implementations - exfatprogs and exfat-fuse are used only
     as test tools, on the PC);
   - the USB mouse report parser (`alice_hid.c`, from the USB HID 1.11 specification);
   - the WiFi (`third_party/Emu68/src/alice/`, run by the kernel on CPU1) and
     `alice68net.device` (`amiga/net/`), written for Alice68 from Linux brcmfmac and
     OpenBSD bwfm (ISC) as references; no code from other Amiga or RISC OS WiFi drivers
     was used.
     `netdev.h` is lwip-amiga's (BSD-3-Clause), copied unchanged;
   - `Alice68-Network` (`amiga/netgui/`), built on the MUI layer of Riptide's RiptidePrefs
     (GPL-2.0-only, https://github.com/farsonic/riptide at `4e5f73c`), copied into
     `amiga/netgui/rt/` with the changes listed in its README;
   - the licensing documents.

Later Emu68 changes are taken on the `upstream` branch and merged into `next`:

- `582253e` (Lukasz Winczura): private LINE-F data-cache range opcodes, the `/emu68`
  `dcache-range-ops` property and 68040.library 2.0. From rondoval's pull request
  michalsc/Emu68#334, which Emu68's `1_1_wip` branch carries; not yet in an Emu68 release.
  Its `start.c` hunk was placed by hand. `1_1_wip`'s follow-ups `9827e5b` (a refactor)
  and `06d88f7` (the same opcodes for the interpreter, which this tree does not have)
  are not needed here.

`git log -p` from commit 2 onwards shows every change made to the upstream
files. `third_party/Emu68/AGA-PISTORM-SOURCE.txt` is AGA-PiStorm's own note.

`docs/EMU68-CHANGES.md` (made by `tools/emu68-changes.py`) lists every Emu68 file that differs
from upstream `0ba3a89`, layer by layer, with the reason for each.

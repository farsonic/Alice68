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
   - the licensing documents.

`git log -p` from commit 2 onwards shows every change made to the upstream
files. `third_party/Emu68/AGA-PISTORM-SOURCE.txt` is AGA-PiStorm's own note.

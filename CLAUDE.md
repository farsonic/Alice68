# Alice68 — notes for Claude

Standalone AGA machine on a Raspberry Pi 4: Emu68 (68k JIT, MPL-2.0) + AGA-PiStorm 1.120
(virtual chipset, GPL-2.0-only) + FRF's EMU68AGA Bare Metal layer + Alice68's own parts.
User-facing docs: README.md. History of where code came from: docs/PROVENANCE.md.

## Layout

- `third_party/Emu68/` — Emu68 at upstream `0ba3a89` with AGA-PiStorm's, FRF's and
  Alice68's changes, vendored (not a submodule). Its `external/` capstone, libdeflate and
  tiny-stl ARE submodules: `git submodule update --init`.
- `core/`, `arm/` — AGA-PiStorm's chipset core and ARM glue (`arm/aga_glue.c` has the FRF
  `FRF_VIRTUAL_AGA` hooks).
- `third_party/Emu68/src/frf_usb/xhci/` — Alice68's Pi 4 xHCI (VL805) driver
  (`frf_xhci.c`), Neptune pass-through (`frf_xhci_pt.c`, `alice68_usbpt.h`), virtual eject.
- `third_party/Emu68/src/raspi/alice_splash.[ch]` — the boot splash (header, status strip,
  SYSTEM/USB log panels). Wired in from `start_rpi64.c` under `ALICE_SPLASH`.
- `amiga/` — 68k side: `alice68-usb.device`, `Alice68-USBLog`, `amiga/hdf/` (`alice68hdf.device`,
  `hdf_core.[ch]`, `Alice68-HDF`), `amiga.h` (minimal NDK types with offset asserts), `lvo.S`.
- `tools/hdf-test/`, `amiga/test/` — tests. `tools/splash-preview/` — render the splash on a PC.

## Build and test

```bash
export ARM_GNU_TOOLCHAIN=$HOME/toolchains/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64_be-none-linux-gnu
bash amiga/build.sh                      # 68k parts -> amiga/out/; regenerates src/boards/alice68hdf.device.h
bash tools/build-emu68aga-pi4.sh         # -> dist/pi4/kernel8.img   (FRF_USB=OFF for the no-USB kernel)
tools/hdf-test/run.sh                    # hardfile tests: core on the PC + driver on a 68k emulator (expect all "ALL OK")
python3 amiga/test/harness.py amiga/out/alice68-usb.device   # USB pass-through driver on the 68k emulator
tools/splash-preview/build.sh > out.ppm  # the splash, drawn by the kernel's own code
```

- Kernel needs the **Arm GNU 15.x big-endian** toolchain (GCC 14 miscompiles vector code).
  68k parts need `gcc-m68k-linux-gnu`; tests need `mkfs.fat`, `mtools`, `sfdisk`, Python
  `machine68k`, Pillow.
- After changing anything in `amiga/hdf/`, run `amiga/build.sh` and **commit the regenerated
  `third_party/Emu68/src/boards/alice68hdf.device.h`**: the kernel embeds it.
- The kernel build dir is fixed (`/tmp/emu68aga-pi4-build`); builds overwrite each other.
- Before declaring something done: fresh clone → submodules → `amiga/build.sh` (git status
  must stay clean) → tests → both kernels build. Nothing here can boot a Pi: say what was
  tested where (PC, emulator) and what only hardware can confirm.

## Things that bite

- **The kernel is big-endian AArch64.** Device registers and DMA structures are
  little-endian: use `LE32`/`rd32`/`wr32` (frf_xhci.c). Never assume host order in
  on-disk formats; hdf_core reads byte by byte so it runs on the PC too.
- **Core roles** (Alice68 on a Pi 4): CPU0 the 68k JIT, CPU1 USB (`frf_xhci_main`, never
  returns; with `frf_usb=neptune` CPU1 runs FRF's TinyUSB on USB-C instead), CPU2
  HVS/HDMI, CPU3 the AGA chipset. `ppc_enable` is forced off.
- **xHCI:** never issue a command from inside another command's wait. Port-change events
  are deferred (`port_pending`) and handled from the main loop. DMA pool: 2 MB below 1 GB,
  first MB uncached (rings, contexts), second MB cached bounce buffers with explicit cache
  maintenance (`pt_dc`).
- **`alice68hdf.device` runs from Emu68's ROM board, mapped read-only**: no writable static
  data (build.sh fails if `.data`/`.bss` appear); all state lives in the device base. It
  starts during Kickstart's expansion setup, after `brcm-emmc.device` (priority 10 < 20):
  `dos.library` is not up (own hunk loader), `FileSystem.resource` may not exist yet.
- **68k code:** call libraries through `lvo_call` (lvo.S). Do not use register-variable
  inline asm: gcc reused a pinned register and corrupted the stack once.
- **68k → kernel log:** Alice68's 68k drivers write lines with `RawPutChar`; `arm/aga_glue.c`
  forwards serial lines starting `[alice68` to `kprintf` (splash SYSTEM panel + serial
  log). The line `[alice68hdf] hardfiles: B bootable, N non-bootable, L left out` is
  **parsed by the kernel** for the status strip: keep its format.
- The splash may only be drawn until 20 s after the 68k starts (`alice_live()`); after that
  the framebuffer may belong to RTG.
- Hardfile rules users rely on: every `.hdf` mounted, none boots unless `hdf.boot=NAME`;
  clashing names → `HF<unit><part>`; a duplicate FFS volume is left out unless chosen; dot
  files (`._*`) ignored; writes only inside the file's own clusters, never the FAT.

## Licensing — keep it clean

- The kernel as a whole is **GPL-2.0-only** (AGA core). Never add GPL-3 code (USPi was
  removed for that reason; CMake errors if its path is requested). Check every new
  dependency's licence and add it to `THIRD_PARTY_NOTICES.md`.
- Emu68's files keep their MPL-2.0 headers; changes to them are MPL-2.0. New Alice68 files:
  `SPDX-License-Identifier: GPL-2.0-only`.
- Never commit Kickstart ROMs, Raspberry Pi firmware, Neptune, disk images or other
  third-party binaries. Original artwork only (`tools/mklogo_alice.py` draws the emblem).
- Don't put anyone's email in code or docs; it belongs only in commit authorship.

## Git and releases

- Work on `next`; fast-forward `main` when the user asks. Keep commits focused, with
  messages that say why.
- This environment can push branches but **cannot push tags or delete remote branches**:
  the user creates release tags on GitHub and attaches the zip.
- Release: update `RELEASE-NOTES.md` (and `ALICE_VERSION` in `alice_splash.h`), commit,
  build from a fresh clone of that commit, then zip `README.md`, `RELEASE-NOTES.md`,
  `SHA256SUMS`, `boot/` (kernel8.img, kernel8-nousb.img, config.txt, cmdline.txt,
  floppy.txt, overlays/), `amiga/` (DEVS/USBHardware/alice68-usb.device,
  C/Alice68-USBLog, C/Alice68-HDF, ReadMe-USB.txt), `Licenses/` and `source/` (git archive
  of the commit plus the three submodules, and SOURCE.txt). The kernels report
  `git:<commit>`; the release must be that commit.
- Upstream: official Emu68 had only a MULU.L/MULS.L fix since `0ba3a89`, already in this
  tree (via AGA-PiStorm 1.120). Faster driver support (data-cache extensions) exists only
  in rondoval's Emu68 fork, not upstream.

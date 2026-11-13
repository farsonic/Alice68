# What Alice68 changes in Emu68

Alice68's kernel is [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz (MPL-2.0), at upstream commit `0ba3a89` (1.1.0-beta.1), changed in four layers. This page lists every Emu68 file that is not as it is upstream, who changed it and why. Emu68's files keep their MPL-2.0 headers and the changes to them are MPL-2.0; new files say their own licence.

Made by `tools/emu68-changes.py` from git, at Alice68 `6a1d43e`. To see any change itself:

- AGA-PiStorm's layer: `arm/emu68-aga.patch` (its patch against Emu68 `0ba3a89`, as released);
- the other layers: `git diff 2b56a31 HEAD -- third_party/Emu68/<file>`, or `git log 2b56a31..HEAD -- third_party/Emu68/<file>` for each commit.

| Layer | What it is | Where |
|---|---|---|
| 1. **AGA-PiStorm** 1.120 (Astair86) | the virtual AGA chipset: Emu68 hooked to it | `arm/emu68-aga.patch`, applied on the `upstream` branch (`2b56a31`) |
| 2. **FRF** EMU68AGA Bare Metal v0.10.44 (Future Retro Fusion) | the standalone machine: virtual CIAs, ROM and ADF boot, Zorro III RAM, TinyUSB on USB-C | commit `1b4c426` |
| 3. **Emu68 `1_1_wip`** | an Emu68 change not yet in a release: data-cache range opcodes (`582253e`) | commit `a3b1681` |
| 4. **Alice68** | the Pi 4 port, the USB card, the network card, the splash, `alice68hdf.device` on the ROM board | the commits since |

**In numbers:** Emu68 at `0ba3a89` has 346 files; **319 of them are exactly as upstream**. AGA-PiStorm's patch changes 15 of them and adds 3; since then 18 files have been changed (10 of them also by AGA-PiStorm) and 5 removed, and 282 added - most of them TinyUSB and the new drivers, in files of their own.

**Alice68 has not changed the 68k translator** (`src/M68k_*.c`, `src/ExecutionLoop.c`): what differs there is AGA-PiStorm's, FRF's and Emu68's own development branch's (this sentence is checked by the script).

## 1. AGA-PiStorm's patch (Emu68 `0ba3a89` to the `upstream` branch)

| File | Lines | What and why |
|---|---|---|
| `CMakeLists.txt` | +79 -2 | builds AGA-PiStorm's chipset core (`core/`, `arm/`) into the kernel |
| `include/M68k.h` | +1 -0 | one field: the interrupt level the virtual Paula asks for |
| `include/logo/logo_aga.h` (new) | +638 -0 | AGA-PiStorm's boot logo (generated) |
| `include/logo/logo_aga_rgb.h` (new) | +8945 -0 | AGA-PiStorm's colour boot splash (generated) |
| `include/support_rpi.h` | +2 -0 | two declarations for the measured clock rate |
| `src/ExecutionLoop.c` | +116 -0 | interrupts from the virtual Paula; a counter of translation units run; the chipset core using the bus |
| `src/M68k_LINE4.c` | +8 -0 | RESET handling for the virtual machine |
| `src/M68k_MULDIV.c` | +4 -0 | an Emu68 fix for 64-bit MULx results (Emu68 `874c3a6`, carried by AGA-PiStorm) |
| `src/M68k_Translator.c` | +8 -0 | counts translations (for the sandbox's statistics) |
| `src/aarch64/mmu.c` | +67 -2 | reserves the virtual chip RAM and the GPU's room after the output frames |
| `src/aarch64/start.c` | +208 -0 | starts the chipset core on its own core; the virtual chip RAM; the RGB-port output |
| `src/aarch64/vectors.c` | +482 -4 | the custom chips' registers ($DFFxxx) go to the virtual chipset; WHDLoad's hold on interrupts |
| `src/cache.c` | +24 -0 | cache handling for the virtual chip RAM |
| `src/pistorm/ps_classic_protocol.c` | +241 -42 | a locked bus API shared with the chipset core; Ctrl-Amiga-Amiga with the bus wedged |
| `src/pistorm/ps_protocol.c` | +261 -42 | the same for the PiStorm32 protocol |
| `src/raspi/start_rpi64.c` | +74 -1 | AGA-PiStorm's splash and logo |
| `src/raspi/support_rpi.c` | +48 -0 | asks the firmware for the clock rate it really measures |
| `toolchains/aarch64_be-arm-gnu.cmake` (new) | +27 -0 | a toolchain file for the Arm GNU big-endian compiler |

## 2. Changed since: files that were there

Lines per layer (added / removed), from each commit; a file changed by AGA-PiStorm too is marked *.

| File | FRF | Emu68 `1_1_wip` | Alice68 | What and why |
|---|---|---|---|---|
| `AGA-PISTORM-SOURCE.txt` (removed) | +0 -7 |  |  | **FRF:** AGA-PiStorm's note about where its Emu68 came from; removed in the merge (it describes AGA-PiStorm's package, not this tree; `docs/PROVENANCE.md` says the same) |
| `CMakeLists.txt` * | +174 -0 |  | +21 -25 | **FRF:** the `FRF_VIRTUAL_AGA` build: the standalone machine, TinyUSB, the virtual CIAs, the Zorro III RAM board<br>**Alice68:** builds Alice68's xHCI driver, Neptune pass-through, splash and network card (`src/alice/`) |
| `build-scripts/README.md` (removed) | +0 -27 |  |  | **FRF:** Emu68's Docker build scripts, removed in the merge (Alice68 builds with `tools/build-alice68-pi4.sh`) |
| `build-scripts/build-aarch64` (removed) | +0 -6 |  |  | **FRF:** as above |
| `build-scripts/build-m68k-amigaos` (removed) | +0 -6 |  |  | **FRF:** as above |
| `build-scripts/build-with-docker` (removed) | +0 -69 |  |  | **FRF:** as above |
| `cmake/verstring.cmake` |  |  | +19 -4 | **Alice68:** the version string names the kernel Alice68 (`ALICE_VERSION`), "based on Emu68" and its version; the `/emu68` device-tree node stays |
| `include/logo/logo_aga_rgb.h` * | +6270 -8943 |  |  | **FRF:** FRF's boot picture in place of AGA-PiStorm's (generated) |
| `include/mmu.h` | +1 -0 |  |  | **FRF:** declares `mmu_unmap()` |
| `src/ExecutionLoop.c` * | +9 -2 |  |  | **FRF:** standalone: no physical interrupt lines, only the virtual chipset's |
| `src/M68k_LINE4.c` * | +24 -0 |  |  | **FRF:** the 68k RESET instruction resets the virtual machine (and re-runs autoconfig) instead of pulsing the PiStorm's reset line |
| `src/M68k_LINEF.c` |  | +76 -0 |  | **Emu68:** private LINE-F opcodes for data-cache maintenance over an address range (Emu68 `582253e`, Lukasz Winczura, from Emu68's `1_1_wip`), which rondoval's `-rangeops` drivers use |
| `src/M68k_SR.c` |  | +5 -0 |  | **Emu68:** as above: the supervisor check for those opcodes |
| `src/aarch64/mmu.c` * | +25 -0 |  | +32 -0 | **FRF:** mapping for the standalone machine<br>**Alice68:** the Pi 4: a 2 MB DMA pool below 1 GB for the xHCI driver (hidden from AmigaOS), and the Zorro III RAM board dropped where the Pi already has RAM at $40000000 |
| `src/aarch64/start.c` * | +188 -12 | +6 -0 | +89 -7 | **FRF:** starts the standalone machine: virtual CIAs, USB on CPU1, the ROM and ADF boot<br>**Emu68:** the `/emu68` `dcache-range-ops` property for the range opcodes (placed by hand)<br>**Alice68:** starts Alice68's work on CPU1 (USB, the network card) and the splash, and stops the splash when AmigaOS draws |
| `src/aarch64/vectors.c` * | +702 -1 |  | +33 -81 | **FRF:** the virtual CIA-A and CIA-B, the standalone machine's view of the custom chips' control registers<br>**Alice68:** which chipset control words a running machine may send (the PiStorm-only ones refused); each CIA's interrupt line kept for the chipset, so a cleared PORTS request is set again while the line is held |
| `src/boards/68040.library.h` |  | +79 -79 |  | **Emu68:** 68040.library 2.0, which uses the range opcodes (generated binary, from Emu68) |
| `src/boards/emu68rom.c` |  |  | +2 -0 | **Alice68:** puts `alice68hdf.device` (hardfiles, SD-card folders) on Emu68's ROM board, beside Emu68's own drivers |
| `src/overlays/diagnostic.dts` | +2 -0 |  |  | **FRF:** a `hud` option for FRF's on-screen display |
| `src/overlays/emu68.dts` | +13 -0 |  |  | **FRF:** joystick button mapping options |
| `src/pistorm/ps_classic_protocol.c` * |  |  | +24 -0 | **Alice68:** no PiStorm on the header: the serial log goes out on GPIO 14 (header pin 8) |
| `src/raspi/start_rpi64.c` * | +684 -4 |  | +383 -41 | **FRF:** the standalone start: no PiStorm probe, the virtual machine set up<br>**Alice68:** the Pi 4 boot: the splash (status strip, log panels, animation; "based on Emu68" from the version string), the boot log, starting the xHCI driver and the network card |
| `src/raspi/support_rpi.c` * | +22 -0 |  | +9 -0 | **FRF:** the boot log on screen<br>**Alice68:** every kernel log line also into the USB card's log ring (for `Alice68-USBLog` and `Alice68-Monitor`) |

## 3. New files

| Files | How many | Lines | Whose | What |
|---|---|---|---|---|
| `include/logo/logo_alice.h` | 1 | 1234 | Alice68 | Alice68's emblem (generated by `tools/mklogo_alice.py`) |
| `src/alice/` | 7 | 3854 | Alice68 | Alice68's network card: WiFi (SDIO host, chip bring-up, FullMAC protocol) and Gigabit Ethernet (GENET) |
| `src/boards/alice68hdf.device.h` | 1 | 35914 | Alice68 | `alice68hdf.device`, built from `amiga/hdf/` and embedded in the ROM board (generated) |
| `src/frf_usb/xhci/` | 5 | 2074 | Alice68 | Alice68's xHCI (VL805) driver for the Pi 4's USB-A ports, the Neptune pass-through (the USB card) and the virtual eject |
| `src/raspi/alice_splash*` | 2 | 515 | Alice68 | Alice68's boot splash |
| `scripts/` | 8 | 163 | FRF (EMU68AGA Bare Metal) | FRF's build and test scripts, and its joystick map |
| `src/boards/frf_z3ram.c` | 1 | 87 | FRF (EMU68AGA Bare Metal) | FRF's Zorro III RAM board |
| `src/frf_net/` | 6 | 472 | FRF (EMU68AGA Bare Metal) | FRF's network bridge board and SDIO probe (built only with `FRF_NET_Z2_BRIDGE` / `FRF_WIFI_SD1_ENUM`, both off in Alice68) |
| `src/frf_usb/` | 7 | 276 | FRF (EMU68AGA Bare Metal) | FRF's USB board and joystick mapping |
| `src/frf_usb/native/` | 9 | 1177 | FRF (EMU68AGA Bare Metal) | FRF's USB keyboard handling and native DWC2 code |
| `src/frf_usb/tinyusb/` | 12 | 2551 | FRF (EMU68AGA Bare Metal) | FRF's TinyUSB glue: keyboards, mice, pads, USB mass storage |
| `src/frf_wifi/` | 2 | 162 | FRF (EMU68AGA Bare Metal) | FRF's WiFi probes (not built in Alice68) |
| `tests/` | 25 | 1992 | FRF (EMU68AGA Bare Metal) | FRF's tests |
| `third_party/tinyusb/` | 196 | 96893 | FRF (EMU68AGA Bare Metal) | TinyUSB, the USB host stack FRF uses on the Pi's USB-C port (MIT) |

## Keeping it so

- New Emu68 and AGA-PiStorm releases come in on the `upstream` branch (Emu68 plus AGA-PiStorm's patch, nothing of Alice68's) and are merged into `next`; CLAUDE.md's "Bring everything up to date" says how.
- New work goes in files of its own (`src/alice/`, `src/frf_usb/xhci/`, `src/raspi/alice_splash.c`), so Emu68's own files change as little as possible and merges stay small.
- After a merge, run `python3 tools/emu68-changes.py` and commit this page: it stops if a changed file has no note.

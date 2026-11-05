# Third-party notices and credits

Alice68 is a standalone Raspberry Pi 4 build of other people's work. It claims
authorship only of its own additions (listed at the end) and preserves every
upstream copyright line, licence text and file header. Where this file and a
file's own header disagree, the header wins.

## What is in the kernel (kernel8.img)

| Component | Authors | Licence | Where |
|---|---|---|---|
| **Emu68** 1.1.0-beta.1 (upstream commit `0ba3a89`) plus the dcache range opcodes (`582253e`, Lukasz Winczura, from Emu68's `1_1_wip`) | Michal Schulz and contributors | MPL-2.0 | `third_party/Emu68` ([upstream](https://github.com/michalsc/Emu68)) |
| **AGA-PiStorm** 1.120: virtual AGA chipset, HDMI output, audio, glue | Astair86 and contributors | GPL-2.0-only | `core/`, `arm/`, `tools/`, `package/` ([project page](https://astair86.itch.io/aga-pistorm-10-experimental-prototype)) |
| **UAE / WinUAE** code inside the chipset core | Bernd Schmidt, Alessandro Bissacco, Toni Wilen and the UAE authors | GPL | `core/src` (headers), [package/UAE-WinUAE-notice.txt](package/UAE-WinUAE-notice.txt) |
| **EMU68AGA Bare Metal**: standalone layer (virtual CIAs, bus stubs, Z3 RAM, ROM loading, Pi 3 USB host) | Future Retro Fusion | GPL-2.0-or-later (FRF-original), MPL-2.0 (changes to Emu68 files) | `third_party/Emu68/src/frf_usb`, `src/boards/frf_z3ram.c`, FRF hunks in `arm/` ([upstream](https://github.com/futureretrofusion/EMU68AGA-Bare-Metal)) |
| **PiStorm** heritage (bus protocol, CPLD loader) | Claude Schwarz (captain-amygdala), Niklas Ekström and contributors | MIT | `third_party/Emu68/src/pistorm` |
| **TinyUSB** (DWC2 host, the USB-C port) | Ha Thach (tinyusb.org) and contributors | MIT | `third_party/Emu68/third_party/tinyusb` (LICENSE kept) |
| **Capstone** (M68K disassembler from Musashi) | Nguyen Anh Quynh and contributors; Karl Stenerud (Musashi) | BSD-3-Clause, LLVM, MIT | `third_party/Emu68/external/capstone` (submodule) |
| **libdeflate** | Eric Biggers | MIT | `third_party/Emu68/external/libdeflate` (submodule) |
| **tiny-stl** | Michal Schulz | MPL-2.0 | `third_party/Emu68/external/tiny-stl` (submodule) |
| **libxsvf** | Clifford Wolf, RIEGL Research | ISC | `third_party/Emu68/src/pistorm/libxsvf` |
| maths library | musl (Rich Felker et al.), FreeBSD msun (Sun Microsystems) | MIT, Sun permissive | `third_party/Emu68/src/math` |
| TLSF allocator, Topaz font, exec lists/nodes | The AROS Development Team | AROS Public License 1.1 | `third_party/Emu68/src/tlsf.c`, `src/raspi/topaz.c`, `include/lists.h`, `include/nodes.h` |
| Emu68 ROM modules and other binary parts in `src/boards/*.h` | Michal Schulz | see [Emu68-tools](https://github.com/michalsc) | carried as data by Emu68 |
| `68040.library` 2.0 in `src/boards/68040.library.h` | Michal Schulz, Lukasz Winczura | MPL-2.0 | carried as data by Emu68 ([source](https://github.com/michalsc/68040.library)) |

## Followed, not copied

These were read as references for sequences, protocols or data; no source
was copied from them, but they deserve the credit:

* **rondoval** – [emu68-pcie-library](https://github.com/rondoval) (GPL-2.0-only)
  and [emu68-xhci-driver](https://github.com/rondoval) (GPL-2.0-only): the BCM2711 PCIe
  bring-up order and the VL805 firmware-reload mailbox call. U-Boot's
  `pcie_brcmstb.c` (GPL-2.0+) documents the same sequence.
* **Neptune / Poseidon** – the `USBHardware` device interface that
  `alice68-usb.device` implements, so that the Neptune stack
  ([rondoval/neptune-usb-stack](https://github.com/rondoval/neptune-usb-stack),
  AROS Public License; Poseidon by Chris Hodges and the AROS Development Team)
  can use the Pi 4's USB-A ports. Alice68 does not include or distribute Neptune.
* **usb_modeswitch** (Josua Dietze, GPL-2.0) – the Realtek RTL815x "driver CD to
  network adapter" switch message used by the virtual eject.
* **xHCI specification** (Intel) and the USB 2.0/3.x specifications.
* **py-videocore6** (Idein Inc., MIT) – the V3D assembler AGA-PiStorm's
  `tools/v3d` uses at build time (`tools/v3d/_videocore6`).
* **Linux brcmfmac** (Broadcom Corporation and contributors, ISC) and **OpenBSD bwfm**
  (Copyright (c) 2010-2016 Broadcom Corporation, Copyright (c) 2016,2017 Patrick Wildt,
  ISC) – the references for `alice68wifi.device`: the SDIO bring-up sequence, the chip's
  register and core layout, the SDPCM/BCDC protocol, the firmware's message layouts and
  the 43455 watermark settings. The driver is new code (`amiga/wifi/`); where it follows
  their sequences closely, their ISC permission notice applies: "Permission to use, copy,
  modify, and/or distribute this software for any purpose with or without fee is hereby
  granted, provided that the above copyright notice and this permission notice appear in
  all copies. THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES..."
* The SD Host Controller and SDIO Simplified Specifications (SD Association) for the
  Arasan host and the SDIO commands.
* `gic400.library` and `mailbox.resource` (Michal Schulz, rondoval; MPL-2.0), which
  Emu68's ROM provides: `alice68wifi.device` calls them, it does not contain them.
* **DejaVu Sans** fonts (Bitstream Vera licence) – rendered once by
  `tools/mklogo_alice.py` into the splash picture; no font file is shipped.

## In alice68wifi.device

* `amiga/wifi/netdev.h` – the netdev driver interface of
  [lwip-amiga](https://github.com/rondoval/lwip-amiga), copied unchanged. BSD-3-Clause,
  Copyright (c) 2026, lwip-amiga contributors; the licence text is in
  `amiga/wifi/LICENSE-netdev.txt`.

## Not included

Kickstart ROMs and other AmigaOS files, the Raspberry Pi firmware
(`start4.elf`, `fixup4.dat`, `bcm2711-*.dtb` – from
[raspberrypi/firmware](https://github.com/raspberrypi/firmware)), Neptune and
its classes, TCP/IP stacks, the WiFi chip's firmware (Cypress/Broadcom), games. You
supply these.

## Licence compatibility notes

* **USPi removed.** FRF's Pi 3 USB host used USPi, which is GPL-3.0-or-later.
  GPL-3 code cannot be distributed in one binary with the GPL-2.0-only chipset
  core (FRF documents this in its `USB-LICENSING.md`). Alice68's Pi 4 kernel
  never linked it, and the USPi sources and their glue have been deleted from
  this repository; asking CMake for that path is an error.
* **AROS Public License files.** Emu68 carries a few AROS files (above). The
  APL 1.1 is based on the MPL 1.1, which the FSF lists as GPL-incompatible.
  This combination is inherited from Emu68 and AGA-PiStorm, which already
  distribute GPL-2.0 kernels containing them; Alice68 neither adds nor
  removes it. Replacing those three small components (allocator, font, list
  macros) with GPL-compatible ones would remove the question entirely.

## Alice68's own additions (GPL-2.0-only)

* `third_party/Emu68/src/frf_usb/xhci/` – the Pi 4 xHCI (VL805) driver, the
  USB pass-through to AmigaOS and the virtual eject.
* `third_party/Emu68/src/raspi/alice_splash.[ch]`, `include/logo/logo_alice.h`,
  `tools/mklogo_alice.py`, `tools/splash-preview/` – the boot splash.
* `amiga/` – `alice68-usb.device`, `Alice68-USBLog`, `alice68hdf.device` and
  `Alice68-HDF` (68k), and the 68k test harnesses.
* `amiga/wifi/` – `alice68wifi.device` and `Alice68-WiFi` (except `netdev.h`, above).
* `tools/hdf-test/` – test SD card images and the hardfile tests; `tools/wifi-test/` –
  the WiFi protocol tests.
* `tools/build-emu68aga-pi4.sh` and the Pi 4 changes to the files above
  (changes inside Emu68's MPL-2.0 files are MPL-2.0).

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
- `amiga/wifi/` — `alice68wifi.device` (Pi 4 WiFi as a lwip-amiga netdev device) and
  `Alice68-WiFi`: `sdio.c` Arasan host, `chip.c` bring-up, `fmac.c` protocol (host-independent,
  tested by `tools/wifi-test`), `wifi.c` device/process/netdev/connecting. Written from Linux
  brcmfmac and OpenBSD bwfm only: **never copy from WiFiPi.device** (no licence on most files).
  Firmware messages are built byte by byte: the 68k gcc aligns 32-bit fields to 2, the chip to 4.
- `tools/hdf-test/`, `amiga/test/` — tests. `tools/splash-preview/` — render the splash on a PC.

## Setting up a fresh machine (Ubuntu 24.04)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build device-tree-compiler xxd \
    gcc-14-powerpc-linux-gnu g++-14-powerpc-linux-gnu gcc-m68k-linux-gnu \
    dosfstools mtools fdisk python3-pil python3-numpy
pip install machine68k==0.3.0          # the 68k emulator the driver tests run on
mkdir -p ~/toolchains && cd ~/toolchains
curl -LO https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64_be-none-linux-gnu.tar.xz
tar xf arm-gnu-toolchain-15.2.rel1-x86_64-aarch64_be-none-linux-gnu.tar.xz
```

Then, in the repository, `git submodule update --init`.

## Build and test

```bash
export ARM_GNU_TOOLCHAIN=$HOME/toolchains/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64_be-none-linux-gnu
bash amiga/build.sh                      # 68k parts -> amiga/out/; regenerates src/boards/alice68hdf.device.h
bash tools/build-emu68aga-pi4.sh         # -> dist/pi4/kernel8.img   (FRF_USB=OFF for the no-USB kernel)
tools/hdf-test/run.sh                    # hardfile tests: core on the PC + driver on a 68k emulator (expect all "ALL OK")
python3 amiga/test/harness.py amiga/out/alice68-usb.device   # USB pass-through driver on the 68k emulator
python3 amiga/test/harness_netcheck.py amiga/out/Alice68-NetCheck   # the network check tool against a fake SANA-II driver
tools/wifi-test/run.sh                   # WiFi protocol layer on the PC (expect "ALL OK")
python3 amiga/test/harness_wifitool.py amiga/out/Alice68-WiFi   # the WiFi tool against a fake driver
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
- The splash may only be drawn until the 68k starts (`alice_live()`); after that the
  framebuffer may belong to RTG.
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
  DEVS/alice68wifi.device, C/Alice68-USBLog, C/Alice68-HDF, C/Alice68-NetCheck, C/Alice68-WiFi,
  ReadMe-USB.txt), `Licenses/` (with amiga/wifi/LICENSE-netdev.txt) and `source/` (git archive
  of the commit plus the three submodules, and SOURCE.txt). The kernels report
  `git:<commit>`; the release must be that commit.

## "Bring everything up to date"

When the user says this (or "update everything", "check upstream"), do all of it, in order,
without asking for each step. Stop and ask only where noted.

1. **Sync.** `git fetch origin`, `git checkout next`, `git merge --ff-only origin/main`
   (`next` must contain `main`), `git submodule update --init`.
2. **Find what's new.** Run `tools/check-upstreams.sh`. It reads `tools/upstreams.tsv`
   (each upstream, the version last reviewed, what it is to Alice68) and prints `ok`,
   `NEW …` with a compare/release link, or `??` for the two checked by hand
   (AGA-PiStorm on itch.io, the Arm toolchain: look at the page).
3. **Take each update in**, one commit (or merge) per upstream, building and testing after
   each:
   - **AGA-PiStorm** (new version on itch.io). Ask the user for the
     `AGA-Pistorm_universal_vX.7z` if you can't download it. Its
     `Source/AGA-PISTORM-X-src.zip` holds the tree, and its `EMU68-SOURCE.txt` names the
     Emu68 commit plus `arm/emu68-aga.patch`. Use the **`upstream` branch**: it is the pure
     upstream import (AGA-PiStorm 1.120 on Emu68 `0ba3a89`, commit `2b56a31`).
     1. `git checkout upstream`.
     2. Replace the AGA files with the new release's.
     3. Rebuild `third_party/Emu68` as Emu68 at the named commit with the AGA patch applied.
        Keep `external/*` as submodule gitlinks at the commits that Emu68 commit records,
        and don't add USPi.
     4. Commit "AGA-PiStorm X on Emu68 <sha>" and push `upstream`.
     5. `git checkout next && git merge upstream`. Conflicts will be where FRF and Alice68
        changed AGA or Emu68 files; keep their behaviour. AGA's `README.md` and `LICENSE`
        live in `docs/AGA-PISTORM-README.md` and `docs/AGA-PISTORM-LICENSE.txt` here: put
        the new ones there and keep Alice68's root files.
   - **Emu68** (`NEW` on `emu68`). Look at every commit in the compare link. Some may already
     be here: AGA-PiStorm carries Emu68 fixes (the MULx fix `874c3a6` came that way). For
     wanted changes: on `upstream`, apply them to `third_party/Emu68`, using
     `git apply -3 --directory=third_party/Emu68` with the upstream diff. Commit, then
     merge `upstream` into `next`. A new Emu68 *release* is better taken together with the
     AGA-PiStorm release built on it.
   - **FRF** (`NEW` on `frf`). FRF's tree is not ours, so port by hand. Read its diff
     (mainly `machine/Emu68/src/frf_usb/`, `src/boards/frf_z3ram.c` and the
     `FRF_VIRTUAL_AGA` hooks in `machine/arm/`). Port fixes that apply to a Pi 4
     standalone machine. Never bring back USPi or anything GPL-3.
   - **Neptune / driver stack** (`NEW` on `neptune`, `driver-stack`). Read their release
     notes for:
     - changes to the `USBHardware` interface or the hub/transfer rules that
       `alice68-usb.device` implements (`amiga/alice68-usb.c`; test with
       `amiga/test/harness.py`);
     - changes to which archive or driver users need (README's Neptune and Ethernet advice);
     - PCIe/xHCI bring-up changes worth mirroring in `frf_xhci.c`.
   - **rondoval-emu68:** don't merge the fork. Its data-cache range opcodes (`582253e`,
     PR michalsc/Emu68#334, also on Emu68's `1_1_wip`) are already in; when an Emu68
     release carries them, nothing more is needed. Keep the `dcache-range-ops` property:
     the `-rangeops` driver archives refuse to load without it.
   - **TinyUSB:** only when FRF moves or a fix is needed (MIT).
   - **Arm toolchain:** try the newer one. Keep 15.x if the kernel misbehaves; GCC 14 is
     known bad.
4. **Verify** (as in *Build and test*): fresh clone of `next`, `amiga/build.sh` leaves git
   clean, `tools/hdf-test/run.sh` all `ALL OK`, `amiga/test/harness.py` `ALL OK`, both
   kernels build, and the splash preview still renders.
5. **Record it.**
   - Set `reviewed` in `tools/upstreams.tsv` to what you reviewed, whether taken in or
     skipped.
   - Update `docs/PROVENANCE.md` and the versions in `THIRD_PARTY_NOTICES.md`.
   - Add an "Unreleased" section to `RELEASE-NOTES.md` saying what changed and what was
     skipped and why.
6. **Push `next`.** Then report:
   - what's new upstream;
   - what was taken in and what was skipped;
   - test results;
   - what needs a real Pi to confirm.

   **Ask before** fast-forwarding `main` or packaging a release, unless the user asked for
   that too.

If nothing is new, say so, with the script's output. Don't make empty commits.

**Running unattended** (GitHub Action from an `@claude` issue, or a web session): you can't
stop to ask. Work on the branch you were given instead of `next`. Push it, and open a
pull request whose description is the report from step 6. Don't fast-forward `main` or
package a release unless the instruction said so. If you need the AGA-PiStorm download and
have no link, say so in the report. How people start these runs:
[`docs/USING-CLAUDE.md`](docs/USING-CLAUDE.md).


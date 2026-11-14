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
  `alice68hdf.device` also mounts SD-card folders (`folders.c` + `fatfs.c`) and CD images
  (`isos.c` + `iso9660.c`, `*.iso` in the root and `ISO/`, read-only, `CD0:`...); the two DOS
  handlers share `dosdefs.h`. `exfat.c`: the card's first exFAT partition (hardfiles, CD images,
  folders and the partition itself as a volume; no 4 GB limit; written only with `alice68.exfat=rw`, the boot volume with `=boot`),
  from Microsoft's exFAT specification. Write order: data, bitmap+FAT, then the entry set (deletes:
  entries first); VolumeDirty while changes are pending; a volume found dirty is validated and repaired
  at boot first (`xfat_check`: pass 1 looks, pass 2 fixes only if everything is fixable); one with
  two FATs or a media failure is never written (`xfat_writable`). Folders on it go
  through the same `ffs_*` calls: `fatfs.c` hands each to `xfs_*` when `v->x` is set; exFAT has no
  `..` entries, so `exfat.c` keeps a table of the directories it has seen (parent, position). Tests
  `tools/hdf-test/exfat.sh` (mkfs.exfat, exfat-fuse, fsck.exfat, power cuts; needs root),
  `harness_isos.py ... exfat` and `harness_folders.py ... exfat|exfatrw|exfatboot [hdfboot]`.
  Folders and the exFAT volume: files in a folder named `S` show the script bit unless a sidecar
  says otherwise (`in_s`), so a Workbench copied from a PC boots.
- `third_party/Emu68/src/alice/` — the kernel's network: `alice_net.h` (the 128 KB Zorro II network
  board, shared with the 68k: 32-bit fields only), `alice_net.c` (board, CPU1 coroutine, commands,
  frame slots, WiFi connection manager, firmware taken from the board, `wifi.*` cmdline options),
  `wifi/sdio.c` Arasan host, `wifi/chip.c` bring-up, `wifi/fmac.c` protocol (host-independent,
  tested by `tools/wifi-test`). CPU1 runs it from the xHCI main loop (`alice_net_poll()`) or alone
  (`alice_net_cpu1_main()`, no-USB kernel). The coroutine must yield at every wait; the kernel
  reserves only x12 (and v19-v26) in these files, so the switch saves the full AAPCS64 callee-saved
  set: x19-x29, x30, sp, d8-d15. Written from Linux
  brcmfmac and OpenBSD bwfm only: **never copy from WiFiPi.device** (no licence on most files), nor
  from RISC OS's binary-only WLanBWFM. Firmware messages are built byte by byte.
- `amiga/net/alice68net.c` — `alice68net.device`: netdev client of the network board (unit 0 WiFi,
  unit 1 the kernel's GENET Ethernet, `eth_*` in `alice_net.c`). `amiga/netgui/` — `Alice68-Network` (MUI,
  built with Riptide's MUI layer in `amiga/netgui/rt/`, see its README; `make -C amiga/netgui`
  from `build.sh`); `amiga/netprefs.c` — `Alice68-NetworkGT`,
  the GadTools settings window (`lvoy.S` for calls with a2/a3). `amiga/wifi/` — `Alice68-WiFi`, `wifi_api.h`, `netdev.h`.
  `alice68hdf.device` hands the `WIFI` folder's files to the kernel at boot (`offer_wifi`).
- Mouse wheel: `frf_xhci.c` reads a mouse's report descriptor (`alice_hid.[ch]`, host-independent,
  `tools/hid-test`) and, with a wheel, uses report protocol; the steps go to `pt_hdr.wheel_y/_x,
  mouse_btn` on the USB card, and `amiga/wheel.c` (`Alice68-Wheel`, in WBStartup) sends NewMouse
  raw keys through input.device. Other mice stay in boot protocol.
- Ctrl-Amiga-Amiga: `frf_native_keyboard.c` spots Ctrl + both GUI keys; `frf_xhci_pt.c` sets
  `pt_hdr.reset_req` = 1; `alice68hdf.device`'s vertical blank server (`hdf_vbl`) sets 2 and calls
  `ColdReboot()`; not taken in 2 s: the Pi reboots (`aga_reboot`). Ctrl+LShift+F11: the Pi reboots.
- `amiga/monitor.c` — `Alice68-Monitor`: debugging window (tasks, backtraces, the board's log rings,
  the WiFi driver's `pt_wstat` "where"). The board layout is in `alice68_usbpt.h`; it survives a 68k reset.
- `tools/hdf-test/`, `amiga/test/` — tests. `tools/splash-preview/` — render the splash on a PC.

## Setting up a fresh machine (Ubuntu 24.04)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build device-tree-compiler xxd \
    gcc-14-powerpc-linux-gnu g++-14-powerpc-linux-gnu gcc-m68k-linux-gnu \
    dosfstools mtools fdisk genisoimage exfatprogs exfat-fuse python3-pil python3-numpy
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
bash tools/build-alice68-pi4.sh         # -> dist/pi4/kernel8.img   (FRF_USB=OFF for the no-USB kernel)
tools/hdf-test/run.sh                    # hardfile, folder and CD image tests: cores on the PC + driver on a 68k emulator (expect all "ALL OK")
python3 amiga/test/harness.py amiga/out/alice68-usb.device   # USB pass-through driver on the 68k emulator
python3 amiga/test/harness_netcheck.py amiga/out/Alice68-NetCheck   # the network check tool against a fake SANA-II driver
tools/wifi-test/run.sh                   # WiFi protocol layer on the PC (expect "ALL OK")
tools/hid-test/run.sh                    # the USB mouse report parser on the PC (expect "ALL OK")
python3 amiga/test/harness_wheel.py amiga/out/Alice68-Wheel  # the mouse wheel tool against a fake card and input.device
python3 amiga/test/harness_wifitool.py amiga/out/Alice68-WiFi   # the WiFi tool against a fake driver
python3 amiga/test/harness_monitor.py amiga/out/Alice68-Monitor  # Alice68-Monitor DUMP against a fake board
python3 amiga/test/harness_netprefs.py amiga/out/Alice68-NetworkGT # the GadTools network window against fake libraries
python3 amiga/test/harness_showconfig.py amiga/out/Alice68-ShowConfig # boards, memory and Alice68's cards, faked
python3 amiga/test/harness_alice68net.py amiga/out/alice68net.device # opens of units the kernel does not serve, and close
tools/splash-preview/build.sh > out.ppm  # the splash, drawn by the kernel's own code
```

- Kernel needs the **Arm GNU 15.x big-endian** toolchain (GCC 14 miscompiles vector code).
  68k parts need `gcc-m68k-linux-gnu`; tests need `mkfs.fat`, `mtools`, `sfdisk`, `genisoimage`, Python
  `machine68k`, Pillow.
- After changing anything in `amiga/hdf/`, run `amiga/build.sh` and **commit the regenerated
  `third_party/Emu68/src/boards/alice68hdf.device.h`**: the kernel embeds it.
- The kernel build dir is fixed (`/tmp/alice68-pi4-build`); builds overwrite each other.
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
  files (`._*`) ignored; hardfile writes only inside the file's own clusters, never the FAT.
- Folders (`amiga/hdf/folders.c` + `fatfs.c`, `alice68.folder=` in config.txt, `ALICE68`
  always): the only code that writes the FAT; mounted hardfiles stay read-only through them.
  Amiga attributes FAT can't hold go in FS-UAE/Amiberry-compatible `NAME.uaem` sidecars, names
  in `%xx` (`uaem.c`, tested by `tools/hdf-test/uaem_test.c`); keep that format compatible.
  Tests: `tools/hdf-test/fatfs.sh` (PC, fsck.fat + mtools), `amiga/test/harness_folders.py`.
- CD images (`amiga/hdf/isos.c` + `iso9660.c`): read-only, never written; a mounted image is
  read-only through the folders too. Tests: `tools/hdf-test/iso.sh` (PC, genisoimage),
  `amiga/test/harness_isos.py`.

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
  build from a fresh clone of that commit, then make two zips. `Alice68-X-Pi4.zip` is laid
  out as the SD card's root, so users copy all of it there: kernel8.img, kernel8-nousb.img,
  config.txt, cmdline.txt, floppy.txt, overlays/, Alice68-READ-ME-FIRST.txt (the firmware
  and Kickstart the card must already have: they are not ours to ship), and ALICE68/
  (`amiga/out/ALICE68` from build.sh, plus README.md, RELEASE-NOTES.md, ReadMe-USB.txt,
  SHA256SUMS and Licenses/ with amiga/wifi/LICENSE-netdev.txt and SOURCE.txt).
  Riptide (the user's own TCP/IP stack, GPL-2.0, github.com/farsonic/riptide) goes into
  ALICE68/Riptide unpacked from the user's riptide-X.lha (with Riptide.info), named in
  SOURCE.txt; never commit its binaries here.
  `Alice68-X-src.zip`: git archive of the commit plus the three submodules, and SOURCE.txt.
  The kernels report `git:<commit>`; the release must be that commit.

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
   - Run `python3 tools/emu68-changes.py` and commit `docs/EMU68-CHANGES.md` (it stops if a
     changed Emu68 file has no note: add one). Also after any change to an Emu68 file of its own.
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


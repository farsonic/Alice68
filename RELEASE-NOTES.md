# Alice68 release notes

## Unreleased

- **WiFi (new, not yet tested on a Pi): `DEVS:alice68wifi.device` and `C:Alice68-WiFi`.**
  The Pi 4's built-in WiFi as a `netdev` device for rondoval's lwip-amiga stack. Written
  from scratch for Alice68 with Linux's brcmfmac and OpenBSD's bwfm (ISC) as references:
  the SD host and SDIO, the chip's bring-up and firmware download, its FullMAC protocol,
  scanning, and joining with the firmware's own WPA2/WPA3 supplicant. The card interrupt
  (through `gic400.library`) wakes it; without one it polls. `Alice68-WiFi JOIN "name"
  "password"` sets the network up; `Alice68-WiFi`, `SCAN` and `LOG` show what it does.
  Opening the device leaves the chip off until `START`, `JOIN` or the TCP/IP stack asks
  for it; `START` shows each step, and `STEP=n` in the prefs stops after step n. The tool
  reports a driver that does not answer instead of waiting for ever. The
  chip firmware comes from Raspberry Pi OS (see the README). The protocol layer is tested
  on the PC (`tools/wifi-test`); the hardware side needs a Pi to confirm.
- **Emu68's data-cache range opcodes** (Emu68 `582253e` by Lukasz Winczura, from Emu68's
  development branch `1_1_wip`; not in an Emu68 release yet). The kernel advertises
  `dcache-range-ops`, so the faster **`-rangeops`** archives of rondoval's driver stack
  run: `genet.device` (SANA-II 3.x or netdev 4.x), `xhci.device`, NVMe and VideoCore
  parts do their DMA cache work in one native loop with one barrier, instead of one
  barrier-wrapped operation per 32-byte line.
  The built-in `68040.library` is 2.0, which uses them too. `Alice68-NetCheck` 1.6 no
  longer calls genet.device 4.x wrong for Alice68: it is the driver for lwip-amiga.
- **The boot splash stops drawing when the 68k starts**, instead of 20 s later, so
  nothing is drawn over AmigaOS's or the RTG screen.
- **`C:Alice68-NetCheck`** (new): what a SANA-II network driver reports, as text in a
  Shell, on any AmigaOS (rondoval's `genet-stats` needs 3.2's ReAction classes). For
  the Pi's own Ethernet (`genet.device` 3.x) it shows link rate, MAC address, packet and
  interrupt counters, and says whether interrupts reach AmigaOS. `WATCH` repeats the
  counters every second; `TEST` brings an unconfigured interface up with its factory
  address, sends three broadcast test frames and checks link, hardware and interrupts
  without a TCP/IP stack.

## 0.3.0 — hardfiles on the status strip

Download: `Alice68-0.3.0-Pi4.zip` from the
[v0.3.0 release](https://github.com/farsonic/Alice68/releases).

Upgrading from 0.2.0: replace `kernel8.img` (and `C:Alice68-HDF` if you use it).
Nothing else on the card changes.

- **Hardfiles on the boot splash's status strip**, after INPUT: `HARDFILES 1 bootable 4
  non-bootable`, plus `n left out` in amber when any were skipped (the SYSTEM panel says
  why), or `none`. The hardfile driver sends a summary line; the kernel reads it.
- **macOS metadata files ignored:** the `._Name.hdf` files a Mac writes next to each
  file on a FAT card are no longer taken for hardfiles (they showed up as "left out").

## 0.2.0 — hardfiles on the SD card

Download: `Alice68-0.2.0-Pi4.zip` from the
[v0.2.0 release](https://github.com/farsonic/Alice68/releases). The kernel reports
`git:3038fb3`.

### New: Amiga hardfiles (`.hdf`) on the SD card

Copy WinUAE-style hardfiles onto the card's FAT boot partition, in the root folder or in
a folder called `HDF`. Alice68 mounts them at start, with nothing to install in AmigaOS.
The new driver, `alice68hdf.device`, is built into the kernel's Emu68 ROM board.

- **Both kinds of hardfile:** full-disk images (with a Rigid Disk Block, any number of
  partitions) and single-partition images (a filesystem from block 0). FAT32 and FAT16
  cards, including fragmented files.
- **Every hardfile is mounted, none boots:** your SD card boots as before.
- **Boot from one with a single line** in `cmdline.txt`: `hdf.boot=Workbench`. That
  hardfile boots and the others stay mounted. Name matching ignores case, `.hdf` is
  optional, and `_` stands for a space.
- **No drive twice:**
  - Where a hardfile's partition name is already taken (the SD card's `DH0`, say), it
    is mounted as `HF<unit><partition>`.
  - A partition holding a volume that's already mounted is left out, because AmigaOS
    mixes up two volumes of the same name.
  - The hardfile chosen with `hdf.boot=` wins instead, and the other drive isn't
    mounted that boot.
- **Writable:** writes go into the file's own space on the card and nowhere else, with
  every request checked against the file's bounds. The FAT itself is never changed.
  `hdf.readonly` in `cmdline.txt`, or `dtoverlay=emmc,unit0=ro` in `config.txt`, makes
  hardfiles read-only.
- **Filesystems from the hardfile:** PFS3, SFS or any other filesystem stored in a
  hardfile's partition table is loaded from it, unless a newer one is already loaded,
  and offered to other drives too. FFS comes from Kickstart.
- **Visible while booting:** the driver's messages appear on the boot splash's SYSTEM
  panel and the serial log: cyan for what was mounted, amber for what was left out and
  why.
- **`C:Alice68-HDF`** (new, in `amiga/C`) lists the hardfiles and the driver's log in a
  Shell.
- **`hdf=off`** turns hardfiles off.

### Other changes

- README: a full setup guide for `config.txt`, `floppy.txt` and `cmdline.txt`, with
  examples.
- README: corrected the USB table. By default the kernel drives the USB-A ports and the
  USB-C port is not used; `frf_usb=neptune` swaps that.
- Alice68's 68k drivers can now write to the kernel log, through the serial port.

### Upgrading from 0.1.0

1. Replace `kernel8.img` on the card.
2. Copy `amiga/C/Alice68-HDF` to `C:` if you want the hardfile tool.
3. Check your `config.txt` has `dtoverlay=emmc,unit0=rw` (the default does). Without
   `unit0`, the hardfile driver can't read the card.
4. **Any `.hdf` files already on the FAT partition will now be mounted.** Add `hdf=off`
   to `cmdline.txt` if you don't want that. Back up hardfiles before writing to them
   from Alice68.

### Known limits

- FAT32 holds files of up to 4 GB.
- Only cards with an MBR partition table; GPT isn't supported.
- Change hardfiles from a PC only while the Pi is off.
- Duplicate volume names are only detected for FFS partitions, not PFS3 or SFS.
- Hardfiles can't be mounted or swapped without a reboot yet.

### Testing

- **On a Raspberry Pi 4:** mounting a hardfile, choosing what boots, and writing to a
  hardfile (`Relabel`).
- **On a PC and in a 68k emulator only:**
  - filesystems loaded from a hardfile's partition table;
  - the duplicate-volume rule;
  - the hardfile messages on the splash;
  - the full test suite (`tools/hdf-test/run.sh`: 9 test cards, 17 driver scenarios).

Reports are welcome in [Issues](https://github.com/farsonic/Alice68/issues).

## 0.1.0 — first release

- Standalone AGA on a Raspberry Pi 4, built from Emu68 1.1.0-beta.1, AGA-PiStorm 1.120
  and the EMU68AGA Bare Metal layer.
- **Kernel USB driver for the USB-A ports:** keyboard and mouse from power-on, also in
  WHDLoad. Other USB devices pass through to the Neptune USB stack in AmigaOS with
  `alice68-usb.device`, which handles storage and network adapters (about 108 Mbit/s
  with an RTL8156).
- **"Virtual eject"** for Realtek network dongles that start in driver-CD mode.
- **The Alice68 boot splash,** with live SYSTEM and USB logs.
- **Licensing cleaned up:** USPi (GPL-3) removed; full notices and source.

---

If Alice68 is useful to you, you can [buy me a coffee](https://buymeacoffee.com/farsonic).

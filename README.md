# Alice68

**A standalone AGA machine on a Raspberry Pi 4, Pi 400 or CM4: Workbench and AGA
software on a local HDMI display, with no PiStorm and no motherboard.**

Alice68 is named after Alice, the AGA chip that succeeded Agnus, and the 68k that
Emu68 runs. It is a Pi 4 build of three projects, plus a Pi 4 USB stack of its own:

- **[Emu68](https://github.com/michalsc/Emu68)** by Michal Schulz: the 68k JIT
  (1.1.0-beta.1).
- **[AGA-PiStorm](https://astair86.itch.io/aga-pistorm-10-experimental-prototype)**
  1.120 by Astair86: the virtual Alice/Lisa/Paula, Copper and blitter, with output
  through the VideoCore VI and sound over HDMI.
- **[EMU68AGA Bare Metal](https://github.com/futureretrofusion/EMU68AGA-Bare-Metal)**
  by Future Retro Fusion: the standalone layer (virtual CIAs, ROM and ADF boot,
  Zorro III RAM, a TinyUSB host on the USB-C port).
- **Alice68's own additions:**
  - a kernel xHCI driver for the Pi 4's USB-A ports;
  - USB pass-through to the Neptune USB stack in AmigaOS;
  - a "virtual eject" for USB network dongles that start in CD mode;
  - a boot splash with live logs.

Credits and licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
[LICENSE](LICENSE). Please report Alice68 problems here, not to the upstream projects.

> **Status:** tested on a Raspberry Pi 4. It boots to Workbench at 1080p. A USB-A
> keyboard and mouse work from power-on. A Realtek RTL8156 network adapter on Neptune
> passed traffic at about 108 Mbit/s. Treat everything else as experimental.

## SD card

FAT32 boot partition:

| File | From |
|---|---|
| `kernel8.img` | the release's `boot/` |
| `config.txt`, `floppy.txt`, `cmdline.txt` | the release's `boot/` (edit `floppy.txt` to name your ROM) |
| `overlays/*.dtbo` | the release's `boot/overlays/` |
| `start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb` / `-400` / `-cm4` | [Raspberry Pi firmware](https://github.com/raspberrypi/firmware) (not included) |
| your Kickstart, e.g. `kick31.rom` | **your own** 512 KB A1200 Kickstart 3.x (not included) |

Amiga partitions are 0x76 MBR partitions, as for any Emu68 1.1 card, seen through
`brcm-emmc.device` (`dtoverlay=emmc,unit0=rw`).

## The boot splash

![splash](docs/splash-1080p.png)

The splash has five parts:

- **Header:** the Alice68 emblem and versions.
- **Status strip:** video, ROM, 68k, CPU1, USB and the input devices found.
- **SYSTEM panel:** the kernel log.
- **USB panel:** the USB log.
- **Credits line.**

Failures show in red, so a photo of the screen is usually enough to diagnose a hang.
The panels keep updating until 20 s after the 68k starts. `aga.nobootlog` in
`cmdline.txt` turns the log panels off. `tools/splash-preview/build.sh` renders the
splash on a PC with the kernel's own drawing code.

## USB

| Port | Controller | Driven by |
|---|---|---|
| **USB-A ×4** (and the Pi 400 keyboard) | VL805 xHCI over PCIe | **the Alice68 kernel**: keyboards and mice go straight to the Amiga's keyboard and mouse port, from power-on and inside WHDLoad. Every other device is passed to **Neptune** in AmigaOS. |
| **USB-C** (CM4: its USB 2.0 port) | DWC2 | FRF's TinyUSB host in the kernel: keyboards, mice, pads, `usbmass.device` |

### Neptune on the USB-A ports (storage, network)

1. Install [Neptune](https://github.com/rondoval/neptune-usb-stack) 6.3 **without**
   `xhci.device`.
2. Copy `amiga/DEVS/USBHardware/alice68-usb.device` to `DEVS:USBHardware/` and
   `amiga/C/Alice68-USBLog` to `C:`.
3. Run `USBStart alice68-usb.device 0`.

Neptune sees an 8-port root hub, and every device the kernel doesn't keep for itself
appears on it. `Alice68-USBLog FOLLOW` tails the kernel's and the driver's USB logs in a
Shell.

- **Network adapters:** the Neptune class is the SANA-II driver. For `cdceth.usbclass`,
  give your TCP/IP stack `usbcdceth.device`, unit 0, as a bare name.
- **Virtual eject:** Realtek RTL815x adapters that start as a driver CD (`0bda:8151`)
  are switched to network mode by the kernel. `frf_usb.nomodeswitch` turns this off.
- **Leaving the USB-A ports to AmigaOS:** `frf_usb=neptune` in `cmdline.txt` keeps the
  kernel off them, so rondoval's `xhci.device` can drive them instead.

## Hardfiles (`.hdf`) on the SD card (test, read-only)

Copy WinUAE-style full-disk hardfiles onto the SD card's FAT boot partition. Put them
in the root folder or in a folder named `HDF`. At start, `alice68hdf.device` (in
Emu68's ROM board) finds every `*.hdf` there. It reads each file's Rigid Disk Block
and adds its partitions with their own names and boot priorities, so a Workbench
hardfile can boot.

- **Needs** `dtoverlay=emmc,unit0=ro` (or `rw`) in `config.txt`, which lets the driver
  read the whole card.
- **Phase 1 is read-only.** Writes are refused as "write protected", so a Workbench
  booted from a hardfile behaves like one booted from a write-protected floppy.
- **Full-disk hardfiles only.** The file must start with an `RDSK` block.
  Single-partition hardfiles (starting `DOS\x`) come in phase 2.
- **Filesystems:** FFS (`DOS\0` to `DOS\7`) works from Kickstart. Partitions using
  PFS3, SFS and the like mount only if that filesystem is already loaded, for
  example from an SD card partition. Loading it from the hardfile itself comes in
  phase 3.
- **Name clashes:** a partition whose name is already taken (say `DH0` on an SD
  partition) is mounted as `HF<unit><partition>`, e.g. `HF10`.
- **Limits:** FAT32 caps each file at 4 GB. Copy files only while the Pi is off.
- **Diagnostics:** `C:Alice68-HDF` lists the hardfiles found and the driver's log.
  `tools/hdf-test/run.sh` runs the tests on a PC.

## Picture options (`cmdline.txt`)

| Option | Values | Default |
|---|---|---|
| `aga.look=` | 0 sharp, 1 smooth, 2 TV | 0 |
| `aga.scale=` | 0 4:3, 1 integer, 2 fill, 3 5:4, 4 16:9, 5 16:10, 6 21:9, 7 32:9 | 1 |

AGA-PiStorm's Chipset menu (`agaboot`) sets the rest at run time.

## Serial log

Connect a **3.3 V** USB-TTL adapter's RX to **pin 8** (GPIO 14) and GND to **pin 6**,
then open a terminal at **921600 8N1**:

```bash
picocom -b 921600 /dev/ttyUSB0
```

## Build

Ubuntu 24.04. You need:

- `build-essential cmake ninja-build device-tree-compiler xxd`
- `gcc-14-powerpc-linux-gnu g++-14-powerpc-linux-gnu`
- the Arm GNU Toolchain 15.x for `aarch64_be-none-linux-gnu`
- for the 68k tools: `gcc-m68k-linux-gnu`

Fetch the submodules first:

```bash
git submodule update --init
```

Build the kernel (output: `dist/pi4/kernel8.img`):

```bash
ARM_GNU_TOOLCHAIN=$HOME/toolchains/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64_be-none-linux-gnu \
  bash tools/build-emu68aga-pi4.sh
```

Build `alice68-usb.device` and `Alice68-USBLog` (output: `amiga/out/`):

```bash
bash amiga/build.sh
```

`FRF_USB=OFF` builds a kernel without the ARM-side USB hosts. `third_party/Emu68` is
Emu68 at upstream commit `0ba3a89`, with AGA-PiStorm's, FRF's and Alice68's changes;
`docs/PROVENANCE.md` says exactly which.

The original AGA-PiStorm README and licence are in
[docs/AGA-PISTORM-README.md](docs/AGA-PISTORM-README.md) and
[docs/AGA-PISTORM-LICENSE.txt](docs/AGA-PISTORM-LICENSE.txt).

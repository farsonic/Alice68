# AGA-PISTORM

An **unofficial** Emu68 fork: native AGA chipset emulation for PiStorm/Emu68 on an
Amiga 500 - "Amiga on Amiga". Its problems are mine, so keep them out of the Emu68
project's inbox.

The Pi already replaces the 68k. I made it replace Agnus/Alice, Denise/Lisa and
Paula too: a chipset emulator written in C runs on a spare ARM core, the emulated
CPU's chip RAM lives in Pi RAM, custom-register accesses are trapped and served by
it, the picture is scanned out over the Pi's HDMI, and the real A500 chipset keeps
only the keyboard, mouse/joystick, serial and clock.

## Status

**The chipset core is validated frame by frame against WinUAE.** Of the frames
captured from eight AGA games plus a Kickstart boot, a CLI screen and a purpose-
built test program, all match pixel for pixel except a handful whose differences
the evidence puts down to the harness, not the core: the snapshots
are taken at frame start, so a game's mid-frame CPU writes (double-buffer swaps,
sprite pointers) cannot be reproduced on replay. One 0.05% case is the core's
immediate blitter not spending cycles a real one spends. See
`docs/VALIDATION-FINDINGS.md`.

**The Pi side runs on hardware (2026-09-08).** On an A500 with a classic
PiStorm and a Pi 4, sandbox mode enters and leaves cleanly without any MMU
remap: chip RAM is served from the ARM-side copy through Emu68's abort handler.
AmigaOS keeps running on the virtual chipset with interrupts, timers, keyboard
and mouse; an OS screen renders through the virtual copper; and Banshee AGA's
intro is drawn by the emulated chipset with sound through the real Paula. See
`docs/HARDWARE-BRINGUP.md`, "The sandbox works" for what had to be fixed and how
I found it.

**Release 1.120 (2026-10-31).** Game controllers for WHDLoad games, from the
A314 and from the Pi's own USB. The emulated chipset answers for the pads and
the mouse itself - JOYxDAT, POTGOR, the CIA's fire bits and a CD32 pad's shift
register (`arm/aga_pad.c`) - from a block the A314's Pi writes into chip RAM
(a314pad and A314Pad), or from the Pi's USB: Poseidon and rondoval's
xhci.device keep the devices, and for a game's length the kernel borrows the
mouse's and the pad's interrupt IN endpoints (`arm/aga_usb.c`) - a Link TRB at
the driver's next free slot into a ring of its own, Normal TRBs with IOC clear
(the CM4's xHCI has one interrupter, the driver's), each report told by the
sentinel it overwrote, linked back at the leave; no xHCI command at all.
agaboot finds the devices through Poseidon and their report descriptors
(`tools/agaboot/usbfind.c`); the kernel finds them on the xHCI by root port and
route string. Outside a game the driver runs, so the Controller window's tester
only reads: the queued transfer's buffer, at every visit of the housekeeper
core, as the driver frees it the moment the report is copied out. Also: a jump
button and per-game pad modes, the A314 after a WHDLoad game, Position kept on
HDMI with a Framethrower, and the installer's VideoCore.card and Framethrower
fixes.

**Release 1.119 (2026-10-25).** A CRT look drawn by the Pi 4's 3D GPU, and
Frame blend. RGB2RTG's V3D work (its 0.78): Curved screen, Glow and Afterglow
as compute jobs on the V3D 4.2, woken from bare metal (`arm/aga_v3d.c`), its
programs regenerated for the core's 32-bit pixels (`tools/v3d/mkcrt.py`,
py-videocore6's assembler) - pass G the glow buffer, A each line across with
the barrel, the frame before and the glow, C the afterglow, B each HDMI row
with the scanlines and the mask - into a GPU room of 16 MB carved on a Pi 4.
Core 3 runs the chipset and must never wait, so the passes are sent at
present and seen through from the line hook; HDMI sync presents that much
earlier. Frame blend works on a Pi 3 too: a copy of the picture plane at half
alpha, with its own line buffer, pointed at the frame before - the core then
goes round three output pages, so that page is never drawn into while it
shows. Also: scanlines in 1% steps with Whole lines (fill), Beam and CRT
fields; HDMI colours; RTG Image's Default and Keep after power cycle; the
installer asks HDMI's rate, monitor or TV and HDMI sound, and reads each
kernel back after copying it.

**Release 1.118-2 (2026-10-24).** A kernel fix and Emu68's latest fixes. A
68k RESET inside the sandbox resets the emulated chipset (`aga_machine_reset`),
and `aga_reset` makes its level 0 by memset, so the IPL hook never fired: a
VERTB pending at the RESET instruction left Emu68 at level 3 for the Kickstart
that booted. Exec's level-3 handler found no enabled bit, returned, and was
called again, about 700 times a frame with a black screen; CaffeineOS's Reboot
after an AGA boot hung this way on a Pi 3 (C:Reset and exec's ColdReboot both
end in the RESET instruction, and whether a VERTB was pending was a matter of
timing). The reset now drops the level, as sandbox leave already did. From
Emu68 by Michal Schulz: MULx.L clears the upper half of the destination
(874c3a6, issue #347), and from `1_1_wip` the read-fault handler's LDAXR,
FLDD post-index and LDPSW decoding (a5d77f2, fe61d88, 6d63b08) and the LRU
invalidation when verification frees a translation unit (7c457be).

**Release 1.118-1 (2026-10-23).** A kernel fix. Emu68's own interrupts (its
GENET network driver's) reach the 68k as level 6, with EXTER folded into
INTREQR (`src/aarch64/vectors.c`). A KickEmu WHDLoad game's Kickstart (Frozen
Bubble's) enables EXTER for its CIA-B timers and clears it, and the network
interrupt, still asserted, came straight back: 7,011,363 times in 72 s on the
A500, with a black screen and F10 dead. From WHDLoad's takeover until the
sandbox is left they now wait (`aga_arm_hold`, `arm/aga_glue.c`): EXTER stays
out of the shadow INTENA that Emu68's IRQ stub reads and out of INTREQR, the
pending interrupt survives the game's EXTER acknowledge, and
`emu68_arm_release` gives the OS its INTENA back and raises what waited for the
OS's driver. System wide does not hold them.
The same release adds *AGA from boot*: System wide, then one software reboot,
which keeps the emulated chipset (`aga_machine_reset`), so Kickstart starts on
Alice and Lisa and graphics.library builds AGA's screen modes. It hung at first
in 12.9 million level-3 interrupts: the 68k RESET pulses the real reset line,
and Kickstart's INTENA=$7FFF clear reached the real Paula as INTEN/PORTS/EXTER
only, so the real VERTB stayed enabled. Clears now go to the real Paula whole
(`REAL_CLR`, all but the audio bits). A WHDLoad game in an OS that booted this
way stays inside the sandbox (`$5A0D`/`$5A0E`, `aga_game_inplace`); the kernel
counts RESETs inside the sandbox (`SEL_RESETS`), which tells agaboot such an OS
and keeps the reboot to once per power-on. The kick31.s slaves in
`tools/whdslave` now take the A1200 Kickstart image only: the A4000 one loops in
exec's resident scan on Emu68, the A600 one puts Emu68's fast RAM in its memory
list.

**Release 1.118 (2026-10-21).** A sound suite for HDMI. The Sound monitor
(`tools/agaboot/monitor.c`, also `SYS:Utilities/AGA-SoundMonitor`) lends the
kernel a ring in fast RAM, and core 3 writes into it what it writes into HDMI's
ring, plus each Paula voice's output and registers: meters, spectrum, VU,
vectorscope, per-voice scopes with mute and solo, and a WAV recorder that splits a
game's music into its four voices (`rec.c`). An equalizer, crossfeed, balance and
limiter run on the whole HDMI mix in fixed point (`arm/aga_fx.h`: Audio EQ
Cookbook peaking biquads, Q28 with error feedback). The voice mixer places each
voice between left and right: in the core's mix (`core/src/audio.c`, the Amiga's
own output and classic HDMI) and in band-limited Paula, where each channel now has
a band-limited ring of its own and the gains apply at the mix (`arm/aga_blep.h`).
The first version split every step into both sides by the gains. It produced the
same samples, and yet with HDMI sound running it froze the whole Pi in Desert
Dream - every core stopped on its next peripheral access, Ctrl-Amiga-Amiga
included - 3 runs of 3, against none without it. A test kernel with a hang watch
on core 1, the PM watchdog (a reset that keeps the RAM) and a one-line flight
recorder found the trigger but not the cause. Also: scanlines of their own for
NTSC frames ($517n), the 1084S and Custom looks, HDMI sync at 1080p 50.2 Hz with a
new measure when the frame length changes, slow RAM passed through to the real bus
in AGA mode (probed at entry; Exec's MemList runs through it), the installer on
Emu68 Imager cards with an RGB port question, and the CaffeineOS start menu
sub-menu - most of these ported from RGB2RTG.

**Release 1.117 (2026-10-17).** The beam a program reads comes from one snapshot:
the chipset core publishes vpos, hpos, LOF and LOL as one word after every colour
clock, and the "never past this line" limit on the read's lead is taken from that
same word (`beam_publish` in `core/src/internal.h`, `aga_beam_value`). Read field
by field, the value could cross into the next line and the next read be back on
the line before - V8 set, then clear, at line 255: a frame end in the middle of
the frame. Desert Dream ran its frame code twice in such frames, cleared the
screen still being shown and wrapped a dot count it takes down once a frame. The
kernel says its version (diag selector $1803) and agaboot warns in Chipset > Init
when it is older than the installed tools - a reset instead of a power cycle
after installing. From RGB2RTG: the line counter is 11 bits and a frame ends only
where it equals the last line.

**Release 1.116 (2026-10-16).** From RGB2RTG, checked against WinUAE: the sprite
comparator at h*2-6 (Reshoot R's rock layer), attached sprite pairs by the odd
ATTACH bit, no sprite DMA while the bitplane fetch runs, the copper's WAIT wake
outside bitplane slots and colour changes at the write (`core/src`); Size Auto
frames a narrow playfield in the standard screen, where Banshee keeps its
border-sprite panel. CRT look on the HDMI picture (`arm/aga_hvs.c`): a CRT mask
as an unscaled pitch-0 plane, scanline shapes Even/Crisp/Whole lines, Gamma
through one table with a soft-knee Brightness (`aga_tone_build`; the RGB port
takes the curve's inverse), Scale 5:4/16:9/16:10/21:9/32:9, and the RTG Image
window grouped. AHI driver 6.2 (`tools/ahi`): slave priority 30, a 40 ms kernel
start buffer, low latency modes with an adaptive margin, PreTimer/PostTimer.
HDMI sync holds the mode's porch for a frame just too fast (`arm/aga_sync.h`).
The installer knows MultibootOS (each system's config_<os>.txt changed with
config.txt), AGS's and AmiKit's Emu68-VC4.card, never puts an older VideoCore
over a newer one, and has an AmiKit choice: a config.txt without a [gpio17=0]
section gets one of mine at its end, and C:WHDLoad becomes a stand-in
(`tools/agaboot/whdwrap.c`) that enters the sandbox before WHDLoad - whose
ExecuteStartup runs in the background and came too late there; after a WHDLoad
update, run Install again.
A line without bitplane data shows no sprites, as on Denise (WinUAE
`bpl1dat_enable_sprites`): Lupus Reditus' specks under the picture. RTG Image has
a Help button, paged to fit the screen (`tools/agaboot/picture.c`, RGB2RTG's).

**Release 1.115 (2026-10-11).** The RGB port's HAM6, measured and made smoother,
above all on a Pi 3 (`arm/aga_rgbout.c`). Rows that did not change keep their
encoding and the base colours keep their slots across palette changes; the core
calls a `line_done` hook per drawn line, which on a Pi 4 keeps per-line colour
sums and fingerprints so only changed rows are read back; a Pi 3 reads the
finished frame through Emu68's cached alias of RAM (9.2 to 4.8 ms a frame). The
bus write turned out to be the PiStorm's own transaction, about 1.3 us a word
whatever the display's DMA or the GPIO sequence, so on a classic PiStorm the
housekeeper core now writes each frame a few words per visit while core 1
encodes the next (Banshee and Chaos Engine AGA at 50 on a Pi 3 A+). Each real
buffer is a vertical ring: a picture that scrolled up or down is matched against
what the buffer holds (row hashes, shifts up to 200 rows), the bitplane pointers
start at the new ring row and the copper's modulos jump back where the ring wraps
(checked cycle-exact in WinUAE on OCS and ECS, and by a host test of the real
code under `qemu-aarch64_be`), so only the rows that came into view are sent -
Pinball Fantasies AGA from 74 % of its frames to 99 %, 2.4 KB a picture. Frame rate
Auto (a steady 25 while 50 does not get through) is the default, and Palette 32
colours (five planes) is an option. Output and Expert are real sub-menus in
ToolsDaemon, Directory Opus 5 and Workbench 3.1.4/3.2 (`tools/agaboot/menu.h`);
the installer asks a Pi 3 about `temp_soft_limit=70`. Fixes from RGB2RTG: Extra
Half-Brite with the brightness up, and the installer's false warning on
CaffeineOS's `Execute S:WHDLoad-Startup`. `aga.sleep` (timed waits sleep on the
timer's event stream) is an opt-in experiment; it sleeps only when more than two
event periods are left, since core 3's 2.3 us pacing steps cannot wait for one.

**Release 1.114 (2026-10-10).** Sound over HDMI grew up. HiFi sound: an AHI
driver (`tools/ahi/aga-hdmi.c`, carried and installed by agaboot) puts what AHI
programs play into a ring the kernel adds to HDMI's sound, in 24 bits at the
program's own rate (44.1-192 kHz, as far as the screen's EDID allows, or any).
Band-limited Paula: the core logs each channel's level changes with their colour
clock and the kernel builds them from band-limited steps (`arm/aga_blep.h`), so
Paula no longer aliases on HDMI. A Sound window with HDMI's A500/A1200 filters
(WinUAE's model), volume with a soft knee, stereo width and swap; a fixed 96 or
192 kHz rate. RTG Image: Custom crop and Auto (the game's display window,
followed), Sharp with even pixels at any scale (`arm/aga_sharp.h`), HDMI sync
(the HDMI frame stretched to keep pace with the emulated one) and HDMI rate,
per-game pictures for demos too; the RGB port's HAM6 can be dithered. The Pi 3
is faster - three core speed-ups from RGB2RTG (copper WAIT wake-up, playfield
tables and sprite spans, sprite replay), a kernel tuned for the Cortex-A53 and
`temp_soft_limit=70` from the installer. The kernels are now built with the Arm
GNU Toolchain 15.3: GCC 14.2 miscompiles big-endian AArch64 vector code (proven
under `qemu-aarch64_be`; it put dark lines into the Pi 3's Sharp picture).
Fixes: Jim Power's HUD sprites, a DIWHIGH change record that hit colour 50, and
HVS display-list words read until two reads agree.

**Release 1.113 (2026-10-05).** Fixes ported from RGB2RTG, AGA-PISTORM's sister
project, and found there: the data fetch window (`DDFSTOP` past the line end, or
at or before `DDFSTRT` - Wit Premium, Aladdin AGA), a mid-line `BPLCON0` plane
count (Zool AGA), blitter line mode, fill and `BLTBDAT` (`core/src/blitter.c`), the
`COPJMP` read strobe (Bubble Bobble), the NTSC Alice ID and long/short lines,
Paula's `AUDxLEN` 0 and silent modulator channels, the real Paula's crystal
measured for the DAC rings, and 14 defects in the HDMI sound driver
(`docs/HDMI-AUDIO.md`). An ECS Denise given to an AGA program that its slave does
not declare - Wit Premium's red skies came out as rainbow bands - now takes its
24-bit colour pairs the AGA way and gives itself up for AGA once the program
loads them across the colour banks or turns on an AGA fetch with AGA colours
(`ecs_says_aga` in `core/src/playfield.c`; found with a WinUAE capture replayed
through the core). Sound On also writes `hdmi_drive=2` into config.txt, the
installer's config.txt edits keep hand-made lines and refuse a block without its
end, and HAM6 on the RGB port no longer flashes the OS's display when a game ends.

**Release 1.112 (2026-10-03).** Sound over HDMI: the emulated Paula's mix also
goes to the screen's speakers, switched from the Chipset menu (Sound - HDMI
On, off by default, kept in ENVARC). The kernel drives the Pi's own HDMI audio
input (the MAI) the way Linux's vc4 HDMI driver does, from a DMA ring that the
chipset core fills, resampled from Paula's 27,928 Hz to 48 kHz and kept in
step by the DMA's play position (`arm/aga_hdmi.c`). Under Emu68 the firmware
sends no audio infoframe and no N/CTS, so the kernel adds both; it works on the
Pi 4 (108 MHz audio clock) and the Pi 3 (its HSM clock), tested on a CM4 and a
Pi 3 A+. What was probed and why is in `docs/HDMI-AUDIO.md`, with the probe and
test-tone tools in `tools/hdmi/`. The installer gives a PiStorm16 only its
kernel line, and puts the boot partition away safely at the end.

**Release 1.111 (2026-10-02).** The installer also sets up a plain AmigaOS
3.1.4 or 3.2, not only CaffeineOS: it makes and switches on WHDLoad's startup
and cleanup scripts, takes over config.txt's own kernel line, and puts the
Chipset menu in Workbench's own menu bar through AppMenu items
(`tools/agaboot/wbmenu.c`) - tested on an A500 with AmigaOS 3.2.3. A1kmode
keeps the whole machine on the emulated chipset at every boot, for an A1000
that cannot take a Framethrower, and real floppy drives can be read while it is
on (a disk DMA bridge in `arm/aga_glue.c`). Castlevania AGA's far background is
drawn whole: AGA's SSCAN2 repeats a sprite every 256 pixels
(`core/src/sprites.c`, following WinUAE).

**Release 1.110 (2026-09-30).** WHDLoad AGA games and demos play on the A500 in
the sandbox with picture, music and sound effects, and return to the desktop
with F10 - tested with Worms: The Director's Cut, Banshee, Alien Breed: Tower
Assault, Reshoot Proxima 3, Virtual Karting 1 and 2 and the demos Synergy,
Shiva, Nexus 7 and Starstruck. 1.110 adds picture settings on HDMI: a flicker
fixer that weaves an interlaced screen into one steady picture, scanlines for
it, brightness, a TV border that also shrinks the RTG desktop (a new command
in the modified VideoCore.card), NTSC games, and a picture of its own for any game,
tested on an A500 with a CM4. 1.109's installer finds the Emu68 boot partition
on a card that CaffeineOS did not set up, and asks for it when it cannot, and
its kernels have a new boot splash. The game can also go out on the
Amiga's own RGB port, as HAM6 drawn by the Pi's second core into borrowed chip
RAM (`arm/aga_rgbout.c`; off by default, Chipset menu -> Output -> RGB port);
1.108 added a choice of its size (384x286 down to 320x256) and a steady 25 fps,
and `agaboot RUN` puts one non-WHDLoad AGA program on the emulated chipset for
as long as it runs - ideas from Future Retro Fusion's
[PiStorm-AGA-HAM6](https://github.com/futureretrofusion/PiStorm-AGA-HAM6).
1.107 brought the RGB port, RGB2RTG's scale (4:3, integer, fill), scanlines, a
TV look and a Position screen on HDMI, the standard screen's true position,
and Virtual Karting's "hardware chunky" colours. The release notes are in
`package/README.txt`.

## Layout

| path | what |
|---|---|
| `core/` | the chipset emulator: copper, blitter, AGA playfield, sprites, audio, virtual floppy. Portable C11, no dependencies |
| `arm/` | Emu68 glue: register trap, chipset thread, HVS video, Paula audio, the HAM6 copy for the Amiga's RGB port, sandbox switching, plus `emu68-aga.patch` |
| `host/` | `agahost`, which replays a snapshot or draws a test scene, and the unit tests |
| `tools/` | validation harness, the Amiga-side tools and their deployment |
| `dist/` | built artefacts: the Emu68 kernels and the Amiga binaries |
| `docs/` | plan, architecture, Emu68 hook points, hardware bring-up, validation findings |

## Building the host core and tests

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

## Validating against WinUAE

`tools/uaesnap.py` drives `winuae-gdb.exe` (copy it from the vscode-amiga-debug
extension into `tools/winuae-gdb/`), boots a floppy or directory, and captures
frame snapshots: registers, AGA palette, chip RAM, DMA records and a screenshot.

```bash
python tools/uaesnap.py out/snap --launch --adf tools/agatest/agatest.adf --wait 30 --count 8 --every 4
build/agahost -snap out/snap/t1 -out out/t1.png
python tools/compare.py out/t1.png out/snap/t1/screenshot.png -o out/cmp.png
```

Pixels only tell you that something moved. To find out *which cycle*, build the
core with the DMA slot tracer and diff its slots against WinUAE's records:

```bash
cmake -S . -B build-trace -DCMAKE_C_FLAGS=-DAGA_TRACE && cmake --build build-trace
build-trace/agahost -snap out/snap/t1 -out /dev/null -trace ours.agat
python tools/tracecmp.py out/snap/t1 ours.agat
```

Every timing bug found so far was located this way, not from the pixels.

## The Amiga side

Small 68k tools I built with amiga-gcc, so the emulation can be used from the
CaffeineOS desktop:

| tool | what |
|---|---|
| `agaboot` | switch the machine between ECS and AGA, report the mode, build the desktop menu, enter and leave sandbox mode, the RTG Image and RGB port settings |
| `agadisk` | boot or insert ADF images on the virtual floppy, fullscreen or windowed |
| `agawindow` | tell the emulator where its window sits on the RTG screen |
| `mousebtn` | inject mouse events, so a remote session can open and screenshot a menu |

`tools/amiga/agactl.py` installs and checks the lot over caffed, CRC-verified:

```bash
python tools/amiga/agactl.py all --port 7334      # tools, Chipset menu, WHDLoad hooks
python tools/amiga/agactl.py status --port 7334   # what the machine is running
```

## Reference emulators

`third_party/` isn't tracked. WinUAE is the authority on AGA behaviour. I keep
E-UAE alongside it: it holds the older UAE/WinUAE code that much of the core was
adapted from (see Credits), and it says its timing constants out loud in headers
where WinUAE has restructured them into code.

## Credits

- **UAE and WinUAE** (GPL; Bernd Schmidt, Alessandro Bissacco, Toni Wilen and
  the other UAE authors): the chipset core contains their code - some copied,
  more translated into C - and elsewhere follows their way of doing things:
  - blitter: the fill step (copied); normal blits and the line mode
    (translated); the fill table; the cycles per word
  - copper: the classic UAE copper (translated); the WAIT/SKIP compare
  - playfield: the fetch tables, fetch modes, display window, delay offset,
    BSCAN2 modulos and dual playfield split (copied); colour writes, register
    handlers and HAM (translated); UAE's fetch and line model
  - sprites: start/stop and the DMA cycle steps (copied); SPRES and
    VSTART/VSTOP (translated)
  - collisions: the CLXDAT read (copied); the CLXCON masks (translated)
  - custom registers: setclr and a few short lines
  - disk: the DD track length and gap; the DSKBYTR bits
  - audio: four interrupt and timing rules, rewritten; no code copied

  Much of it is the older UAE/WinUAE code as kept in E-UAE (Richard Drummond's
  UAE port), from before WinUAE's 2024-2025 rewrite of custom.cpp and
  drawing.cpp. The timing was checked against WinUAE's DMA records, and WinUAE
  was the reference every frame was compared with. The header of each file in
  `core/src` says what it took, with the copyright lines of the files it came
  from. WinUAE: <https://www.winuae.net>, source
  <https://github.com/tonioni/WinUAE>.
- **PiStorm** by Claude Schwarz (captain-amygdala) and **PiStorm600** by LemaruX -
  the hardware.
- **Emu68** by Michal Schulz, with contributions from CrypticalCode0, MBeijer,
  mras0, rondoval, captain-amygdala, ronaegis, apolkosnik, mdpye and TomCat666 -
  the kernel this is built into (MPL-2.0).
- **WHDLoad** by Wepl - the slaves in `tools/whdslave` are built from his
  kick31.asm.
- Testing: katzazakis, FraBro and Angelclaww.

License: GPL-2.0-only (see LICENSE; the text is in COPYING). The changes
`arm/emu68-aga.patch` makes to Emu68's own files are MPL-2.0, like the files
they change (MPL-2.0.txt), and so are the VideoCore.card changes in
`package/videocore-src`. The kernel as a whole - Emu68 with the chipset built
in - is distributed under the GPL-2.0, as MPL-2.0 section 3.3 allows; its
third-party notices are generated into the release's
`Licenses/Emu68-third-party-notices.txt` by `tools/mkpackage.py`, which also
makes the Emu68 source tarball published next to each release.

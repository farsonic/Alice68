AGA-PISTORM 1.120 - an UNOFFICIAL Emu68 fork (this had many many iterations and now its usable)
      - AGA games on your A500, with a classic PiStorm -
================================================================
by Astair86

The package - and the Emu68 source its kernels are built from - lives on
https://astair86.itch.io/aga-pistorm-10-experimental-prototype

WHAT'S NEW IN 1.120
-------------------
 * Game controllers for WHDLoad games (Chipset menu, Expert,
   Controller...). Games read them as if they were plugged into the
   Amiga's own ports - a mouse in the mouse port, a pad in the joystick
   port as a CD32 pad - with no adapter. They come from:
   - the A314: the pads and the mouse on the A314's Pi, sent by a314pad
     there and A314Pad on the Amiga;
   - the PiStorm USB: a USB pad or mouse plugged into the Pi's own USB -
     a CM4's USB port, or a Pi 4's USB-C port with otg_mode=1 - through
     the drivers you already use, Poseidon and rondoval's xhci.device.
     This package brings no USB driver. A Pi 4's USB-A ports, a Pi 3 and
     a Zero 2 W cannot be read in games: their USB is another kind.
   On Workbench they stay Poseidon's, or A314Pad's, as before.
 * Controllers from: None (the default), A314, PiStorm USB, or both.
 * The PiStorm USB's mouse and pad are found by themselves: the first
   mouse and the first pad Poseidon has, each laid out from what the pad
   itself says about its buttons and sticks, wherever it is plugged in.
   agaboot USBFIND shows what it finds; a pad whose buttons come out
   wrong can be set by hand (agaboot USBPAD, USBMOUSE).
 * A tester in the same window: every direction and button lights up as
   the game would get it, the USB pad's too.
 * A jump button, for games that jump with Up: the button you pick
   gives the game Up. "Game jumps with" for games that jump with a
   button instead - Up jumping too, Up only jumping, or Up staying Up.
   D-PAD Jump is one such setting for every game without its own.
 * A pad can be a CD32 pad (the default), a 2-button stick or a
   1-button stick, game by game: a game that knows the CD32 pad then
   keeps its joystick controls (Turrican 2 AGA).
 * The A314 works after a WHDLoad game: its mouse was dead until the
   next boot.
 * Position stays on HDMI with a Framethrower: its calibration grid gave
   way to the Framethrower's picture as soon as the mouse moved. A user
   with a Framethrower reported it.
 * The installer always puts this package's VideoCore.card in, also over
   a driver with a higher number (AmiKit's 1.9): a driver made for
   another kernel may not know this one. The driver replaced is kept as
   VideoCore.card.pre-aga, which Uninstall puts back.
 * An update could drop the Framethrower from config.txt: the installer
   took the line in its own block for the card's own and did not ask.
   It asks again now, with the last answer as the default.
 * The installer's last window is shorter: its text no longer runs
   behind Proceed.
 * RTG Image's Help and this README: a Framethrower's own scanlines and
   position are set in config.txt, not in RTG Image.
 * Tried on a standalone AmiKit card after AmiKit's own update: Install
   runs through, and AGA games show on HDMI with their sound. If Install
   stops at 75%, or games show only on the RGB port, run AmiKit's update
   first, then install this package again.
 * Install it over 1.119 as usual, then switch the Amiga off and on.

WHAT WAS NEW IN 1.119
---------------------
 * A CRT's look drawn by the Pi 4's 3D GPU (RTG Image, CRT look; a Pi 4
   or CM4 only):
   - Curved screen, 1 to 9: the picture bent like a CRT's glass, the
     scanlines bending with it;
   - Glow, 1% to 100%: the light a CRT's glass spreads around the
     bright parts;
   - Afterglow, Light, Medium or Strong: a phosphor glowing on for a
     moment where the picture went dark, so a moving light leaves a
     trail.
   The GPU draws them after each frame, with the scanlines and the mask
   in it: a few milliseconds before the picture is on HDMI, nothing
   taken from the Amiga. As in RGB2RTG, whose GPU work this is.
 * Frame blend (RTG Image, CRT look; a Pi 3 too): each frame mixed half
   and half with the one before, as a CRT and many RGB monitors blend
   them. Games that flicker on purpose at 50 Hz (Agony's sky, two colour
   fades in turn) or scroll a pixel every other frame (Apidya) look
   steady. On a Pi 3 at 1080p it may be too much: switch it off if the
   picture breaks up.
 * Keep after power cycle: No (the default) - Frame blend and the three
   GPU effects are off again after the next power cycle, even with
   Save, for the one game that wants them; Yes, they stay.
 * Scanlines in 1% steps: Scanlines, Scanlines (interlace) and Scanlines
   (NTSC) are sliders now. The 1084S look uses the monitor's own 27%
   (40% on NTSC), where it had to round to 25% and 50%.
 * New scanline shapes: Whole lines (fill) - every Amiga line the same
   number of HDMI lines and the picture full height, a few lines of the
   border cut; Beam and Beam (fill) - each line darker towards its edges
   as a CRT's beam fades, softer than one dark line. Crisp is as dark
   on every line with 4:3 and Fill screen now.
 * Scanline position: the dark line at the bottom of each Amiga line or
   at its top; or CRT fields, with Flicker fixer Off: an interlaced
   screen's every other field half a line lower, as a CRT draws it.
 * HDMI colours (RTG Image, This Amiga): PC monitor (full range) or TV
   (limited range). Until now 1080p 50 Hz always got full range, which
   a TV shows too dark.
 * Default (RTG Image): every row of Picture and CRT look back as a new
   install has them. Custom's Crop edges and This Amiga's rows stay.
 * RTG Image fits a lower screen: CRT look moves to a second column.
 * The installer asks three more things: HDMI's rate (1080p 50 Hz, 50.2
   Hz for HDMI sync, 60 Hz, or the display's own), a PC monitor or a
   TV, and the Amiga's sound on HDMI too. It also reads each kernel back
   after copying it to the SD card: a card that loses a write (a Pi 4's
   at 50 MHz) no longer gets a broken kernel - the installer tries three
   times, else it stops and the card stays as it was.
 * On MultibootOS the installer, and agaboot CHECK, name the systems on
   the card that still boot their own kernel: AGA-PISTORM goes into one
   system at a time.
 * The RGB port shows the picture again after CaffeineOS's Reboot with
   A1kmode or AGA from boot, and goes black when a game quits back to
   the RTG Workbench with AGA from boot.
 * Install it over 1.118-2 as usual, then switch the Amiga off and on.

WHAT WAS NEW IN 1.118-2
-----------------------
 * With AGA from boot on, CaffeineOS's Reboot (Soft or Hard) could stop
   on a black screen and never come back - and so could any program
   that restarts the Amiga. It depended on the moment: a screen
   interrupt that came in just before the restart stayed raised for the
   Kickstart that started next, which found nothing to answer and was
   called again and again. The restart now clears it. An itch.io user
   with a Pi 3 reported it.
 * Fixes from Emu68, by Michal Schulz:
   - MULU.L or MULS.L followed by a shift of the same register gave a
     wrong result (Emu68 issue #347);
   - three kinds of ARM instructions are decoded correctly when the
     68k's access goes to the chipset or to chip RAM;
   - the JIT can no longer run a piece of translated code it has just
     thrown away.
 * Install it over 1.118-1 as usual, then switch the Amiga off and on.

WHAT WAS NEW IN 1.118-1
-----------------------
 * WHDLoad games that start a Kickstart of their own (Frozen Bubble, for
   one) no longer hang on a black screen while Emu68's network driver
   runs. Its interrupts flooded the game's Kickstart, which has no
   driver for them; now they wait until the game ends, and the network
   carries on after F10.
 * AGA from boot (Chipset menu -> Expert -> System wide -> "AGA from
   boot"): at every power-on the Amiga reboots once by itself, and
   AmigaOS starts on the emulated AGA chipset - with AGA's screen
   modes, PAL 256 colours among them, so AGA programs and demos that
   start from Workbench find what they look for. WHDLoad games run
   inside it as usual. Hold the left mouse button while the Amiga
   starts to skip it once; switch it off in the same requester. Like
   System wide, it can freeze the machine. An itch.io user found it
   by accident, when a crashed demo made an A600 reboot itself.
 * A Kickstart that starts on the emulated chipset - the AGA boot, or a
   WHDLoad game's own - no longer drowns in interrupts the Amiga's own
   chipset kept raising for it. That is what stopped the AGA boot on
   its splash screen.
 * WHDLoad games that start a Kickstart 3.1 of their own need
   kick40068.A1200 in DEVS:Kickstarts. WHDLoad also accepts the A4000
   and A600 images, but on Emu68 those hang the game on a white or
   black screen; the WHDLoad installs made for AGA-PISTORM (Frozen
   Bubble, Reshoot Proxima 3 and others) now ask for the A1200 image
   alone, so WHDLoad says when it is missing.
 * Monkey Island 2 AGA (the 2026 fan version) works. It needs WHDLoad
   20 - CaffeineOS 9317 comes with WHDLoad 19.2.
 * Install it over 1.118 as usual, then switch the Amiga off and on.

WHAT WAS NEW IN 1.118
---------------------
 * A Sound monitor (Chipset menu -> Sound -> Monitor..., or the
   AGA-SoundMonitor icon in SYS:Utilities): level meters and a spectrum
   of what HDMI plays, VU meters, a vectorscope, and Paula's four voices
   each with its own scope - click a voice to mute it, right-click to
   hear it alone. Record writes the sound to a WAV file, and during a
   game each voice to a file of its own as well.
 * An equalizer for sound over HDMI (Sound -> Equalizer...): ten bands
   with presets, headphone crossfeed, balance and a limiter.
 * Paula's voices anywhere between left and right, each at its own level
   (Sound -> Voices...), on HDMI and on the Amiga's own output: Soft
   stereo or Headphones end the hard left and right that tires the ears
   on headphones. See SOUND OVER HDMI.
 * Scanlines of their own for NTSC screens (RTG Image -> Scanlines
   (NTSC)): a CRT shows NTSC's lines stronger (from RGB2RTG).
 * RTG Image: the look of a Commodore 1084S in one click, your own look
   kept as Custom look, and a Help that scrolls (from RGB2RTG).
 * HDMI sync: 1080p at 50.2 Hz, a rate with room above PAL's 50 Hz to
   follow a game's picture smoothly, and a game that changes its frame
   length is measured again (from RGB2RTG).
 * Slow RAM - a trapdoor card's at $C00000, or an A1000's - works in AGA
   mode: AGA titles said "0 graphics mem" or did not start on such an
   Amiga.
 * The installer works on SD cards made with Emu68 Imager, and asks
   whether the Amiga's own RGB port should show the games (HAM6).
 * CaffeineOS: the Caffeine button's start menu gets an AGA-PISTORM
   sub-menu, with a chip for its icon (from RGB2RTG).

WHAT WAS NEW IN 1.117
---------------------
 * Demos and games that time themselves by the raster beam no longer see
   a frame end in the middle of a frame. Desert Dream's part II blinked
   in its lower half, and after the cube it drew every dot it ever had
   in one starburst - with F10 dead until a reset. All of that is gone.
 * Chipset > Init now tells you when the kernel running is older than
   the AGA-PISTORM you installed. That happens when the Amiga is reset
   with Ctrl-Amiga-Amiga after installing instead of being switched off:
   the old kernel keeps running beside the new tools, and things like
   the RGB port can act up. Switch it off and on and the new kernel
   starts. C:agaboot CHECK shows the kernel's version too.
 * A program that moves the beam's line counter past the last line now
   counts on into the next frame the way a real Agnus does, instead of
   ending the frame at once (from RGB2RTG).

WHAT WAS NEW IN 1.116
---------------------
 * More fixes in the emulated chipset, found together with RGB2RTG and
   checked against WinUAE:
   - Reshoot R's rock background is back - it was flat bands of colour
     (a sprite the copper moves along each line);
   - Brian the Lion's clouds and his glove look right;
   - the copper's waits and colour changes land exactly where WinUAE
     puts them, so Shadow of the Beast's sky and Castlevania look as
     they should;
   - Size Auto doesn't blow up games with a narrow playfield any more
     (Banshee in game), and it keeps the score panel;
   - Lupus Reditus no longer shows a row of coloured specks under the
     picture.
 * RTG Image got a CRT look: a CRT mask (the fine dark columns of an
   aperture grille, 25% to 100%), three scanline shapes - Even (the new
   default), Crisp (how it looked up to 1.115) and Whole lines - and
   Gamma: Exact, CRT (the darker mid-tones of a 1084S) or Lighter.
   Brightness now bends towards white instead of clipping everything
   bright.
 * More Scale choices: 5:4 (square pixels), 16:9, 16:10, 21:9 and 32:9.
 * The RTG Image window is tidier: Settings for at the top, then
   Picture, CRT look and This Amiga. And it has a Help button now that
   tells you what every option does.
 * HiFi sound (AHI) no longer crackles while you drag windows around or
   a song starts from the SD card. There's a new choice for games too,
   On, low latency: their sound reaches you about 60 ms sooner.
 * HDMI sync leaves the timing alone when a game runs a hair faster than
   the HDMI mode, instead of fiddling with it over and over.
 * The installer knows more systems:
   - MultibootOS keeps a copy of config.txt for each system, and the
     installer now updates that copy too - so the boot menu no longer
     undoes your install;
   - on AGS and AmiKit their RTG driver (Emu68-VC4.card) is updated as
     well, but if you already have a newer VideoCore driver than ours,
     it is left alone;
   - your RTG screen modes survive the new driver: where they were made
     with an older VideoCore, the installer adds VC4_LEGACY_ID to the
     monitor icon (and Uninstall puts the icon back as it was);
   - AmiKit has a choice of its own. Read AMIKIT AND MULTIBOOTOS,
     especially if you ever update WHDLoad there;
   - on a Pi 4 it asks whether to run the SD card at 25 MHz instead of
     50, for cards that lose a write now and then. The kernel takes
     emmc.low_speed from the command line file now too, as Emu68 1.0.x
     did - MultibootOS cards have it there for every system;
   - a CaffeineOS card shared with an A1200's PiStorm32-lite gets a short
     block at the end of config.txt, so the A1200 always starts its own
     kernel (now and then it could start the classic PiStorm's);
   - HDMI rate 50 Hz brings disable_overscan=1 where config.txt does not
     say, so the picture has no black frame around it (AmiKit had one).

WHAT WAS NEW IN 1.115
---------------------
 * The RGB port - HAM6 on the Amiga's own monitor - is much smoother,
   above all on a Pi 3:
   - only the rows of the picture that changed are read back and turned
     into HAM6 again, and the base colours keep their places when the
     palette changes, so a new palette no longer rewrites the whole
     frame;
   - a Pi 3 reads the finished picture through its cache: 9 ms a frame
     down to 5;
   - the picture goes over the PiStorm from another core of the Pi while
     the next one is being made. On a Pi 3 A+, Banshee AGA and The Chaos
     Engine AGA now run at 50 pictures a second on the RGB port;
   - a picture that scrolls up or down - a pinball table, a vertical
     shooter - is moved on the Amiga's own display the way games scroll
     themselves, and only the rows that came into view are sent. Pinball
     Fantasies AGA went from three pictures in four to 99 in a hundred, at
     50 a second, on a Pi 3 A+;
   - Frame rate Auto is the new default: 50 while 50 gets through, a
     steady 25 while it does not, instead of an uneven 30 or 40;
   - Palette 32 colours, a new choice next to HAM6: about a fifth fewer
     words a frame and no HAM fringes, for a game whose busiest moments
     are a little too much for HAM6. HAM6 stays the default.
   THE AMIGA'S OWN RGB PORT says what limits it.
 * Output and Expert are real sub-menus now - Output: RTG Image... and
   RGB port..., Expert: System wide... and Run program... - in
   ToolsDaemon, Directory Opus 5 and the menu bar of Workbench 3.1.4 and
   3.2. On 3.5 and 3.9, where Chipset is a Tools sub-menu, they open
   their small windows as before.
 * The installer asks a Pi 3 whether to keep its full speed up to 70 C
   (temp_soft_limit=70 in config.txt). Yes is the default; No leaves the
   line out, and the Pi slows down at 60 C as it would without
   AGA-PISTORM.
 * Extra Half-Brite with the Brightness above 100 % (RTG Image): a
   half-bright colour is now the real colour halved, then brightened.
   Before, it was brightened first and then halved, so the half of a
   bright colour came out too dark (found by RGB2RTG).
 * The installer no longer warns that WHDLoad.prefs runs "another
   command" when its ExecuteStartup line reads Execute
   S:WHDLoad-Startup, the way CaffeineOS writes it (found by RGB2RTG).
 * An experiment, off by default: aga.sleep lets the Pi's cores sleep
   through their timed waits instead of spinning (see GOOD TO KNOW).
 * Tested on an A500 with a Pi 3 A+; not on a PiStorm16.

WHAT WAS NEW IN 1.114
---------------------
 * HiFi sound: AHI programs - music players, trackers, the desktop's
   sounds - can play straight on HDMI instead of through Paula's four
   8-bit channels: in 24 bits (16 on a screen that takes no more), at
   the program's own rate, from 44.1 up to 192 kHz. Chipset menu ->
   Sound -> HiFi sound (AHI). It needs AHI installed. Off by default;
   once on, it stays on. See SOUND OVER HDMI.
 * Paula on HDMI, Band-limited: a game's or a demo's sound is built on
   HDMI from Paula's exact steps, so nothing folds back as aliasing -
   cleaner, most of all in high notes and sharp effects. Classic, the
   sound as before, stays the default.
 * Sound rate (HDMI): HDMI follows an AHI program's rate as far as your
   screen says it can; "Follow, any" is for a receiver or monitor that
   plays more than it says, and 96 or 192 kHz holds HDMI there for
   everything, games included.
 * The Sound window: Sound on HDMI, the filter, volume (100 to 400 %),
   stereo width (Amiga, 75, 50, 25 %, Mono) and left and right swapped
   - stereo and swap on the Amiga's own output too. HDMI's filter can
   be an A500's (its fixed filter and the LED filter) or an A1200's (the
   LED filter alone), switched by the power LED as on a real Amiga.
 * RTG Image: Size Custom, with Crop... to cut each edge off the last
   game's picture (PAL and NTSC each their own), and Size Auto - the
   game's own display window, followed as it plays; with Scale Fill
   screen the game fills the display. Both are RGB2RTG's, AGA-PISTORM's
   sister project.
 * Sharp: every Amiga pixel the same width, also where the picture is
   enlarged by no whole number (RGB2RTG's area kernel).
 * HDMI sync and HDMI rate (RTG Image): "Follow the Amiga" makes HDMI's
   frame a line or two longer to keep pace with the game, so a PAL game
   on a 50 Hz screen shows every frame exactly once - no judder; HDMI
   rate puts a 1080p50 or 1080p60 mode into config.txt for it.
 * "Settings for" in RTG Image: Add... gives any game or demo a picture
   of its own by its drawer - demos started with Run program... too.
 * The RGB port: Colours - Exact (rounded, the default) or Smooth
   (dithered: smoother gradients, a little grain).
 * The Chipset menu is shorter: Init, Output (RTG Image..., RGB
   port...), Sound, Expert (System wide..., Run program...), About.
 * The Pi 3 is faster: three of RGB2RTG's speed-ups in the chipset
   emulation and a kernel tuned for the Pi 3's own processor - Jim Power
   skipped 4 frames in three and a half minutes on a Pi 3 A+. The
   installer adds temp_soft_limit=70 for a Pi 3: without it a Pi 3 B+ or
   3 A+ drops from 1.4 to 1.2 GHz at 60 C. Give it a heatsink anyway,
   and a fan if you can.
 * The kernels are built with a newer compiler (the Arm GNU Toolchain
   15.3). The one before, GCC 14.2, got some of the Pi's vector code
   wrong - on a Pi 3 the Sharp picture came out darker, with lines.
 * Fixes: Jim Power's status bar (its heads and icons) is drawn whole;
   a game that changes DIWHIGH in the middle of a line no longer
   changes colour 50; and the HDMI picture's settings are read back
   until two reads agree (a dark picture now and then, found by
   RGB2RTG).
 * Tested on an A500 with a CM4 and with a Pi 3 A+; not on a PiStorm16.

WHAT WAS NEW IN 1.113
---------------------
 * Wit Premium (the AGA demo by Freezers) drew its red skies as rainbow
   bands, and its buildings green instead of blue. Its WHDLoad install
   does not say the demo needs AGA, so it was given an ECS Denise - which
   has no 24-bit colours. Such a game is now watched: colours it writes
   the 24-bit way are taken as AGA, and once it loads them across the
   colour banks, or turns on an AGA fetch mode with AGA colours, it gets
   the whole AGA chipset. ECS games keep their ECS Denise, stray AGA
   bits and all, and Init's "ECS mode" still means ECS for every game.
 * Wit Premium's PREMIUM screen is centred again, its last letter no
   longer cut off, and Aladdin AGA's logo screens are whole pictures
   instead of a narrow striped column.
 * Fixes found by RGB2RTG, AGA-PISTORM's sister project, that apply here
   too: a change of the bitplane count in the middle of a line (Zool
   AGA's status bar), the blitter's line drawing and area fill (the
   demos 9 Fingers, State of the Art and Arte), Bubble Bobble restarting
   the copper by reading its jump register, the NTSC Alice chip ID and
   NTSC's long and short lines.
 * Sound: a sample of length 0 plays 65,536 words, as on a real Amiga,
   instead of one; a channel a game uses to modulate another is silent
   instead of a loud buzz; a game that switches Paula's modulation on no
   longer garbles the real Paula's output; on an NTSC Amiga the sound no
   longer replays a moment of old sound every few seconds (the Amiga's
   crystal is measured at power-on now).
 * Sound over HDMI, 14 fixes: the Pi 3 kernel on a Pi 4 plays too, a
   screen switched off or unplugged no longer risks stalling the Pi,
   and HDMI audio or a DMA channel that another driver already uses is
   left alone. HDMI On also puts hdmi_drive=2 into
   config.txt, which a screen that the Pi drives as DVI needs before it
   plays any sound - see SOUND OVER HDMI.
 * HAM6 on the RGB port: when a game ends, the Amiga's own monitor no
   longer flashes garbage before the Workbench comes back (seen by a
   player who starts games from AGS).
 * The installer: a line that follows the installer's comment in
   config.txt is no longer deleted with the comment when the line is not
   the installer's, and a
   config.txt whose AGA-PISTORM block has lost its end line is left
   alone with a message instead of cut short. It also tells you when
   DEVS:Monitors has no VideoCore monitor icon, which a plain AmigaOS
   3.x needs for the RTG desktop (see WHAT YOU NEED). SD-Setup keeps the
   lines the Amiga's own installer put into config.txt.
 * C:agaboot CHECK says whether the AGA kernel is running and whether
   sound over HDMI is on.
 * This README now covers a plain AmigaOS 3.2 card in more detail: the
   VideoCore monitor and screen modes, unpacking the whole archive, and
   the way back if the Amiga no longer starts.
 * Tested on an A500 with a CM4 (the Pi 4 kernel, and the Pi 3 kernel
   for sound over HDMI); not on a Pi 3 or a PiStorm16 this time.

WHAT WAS NEW IN 1.112
---------------------
 * Sound over HDMI: the emulated chipset's sound - games, demos, the
   whole Workbench in System wide - can play on your screen's speakers
   as well as from the Amiga's own audio output. Chipset menu -> Sound
   -> HDMI On. Off by default; once on, it stays on across reboots.
   Works on the Pi 4 / CM4 and on the Pi 3 - see SOUND OVER HDMI below.
 * On a PiStorm16 the installer changes config.txt's kernel= line and
   nothing else: no clock line (core_freq=500, which a classic PiStorm
   needs, troubled a PiStorm16, which wants 400), no gpu_mem, no
   overlay, and no Framethrower or PAL/NTSC question. The one exception
   is a card with no emmc line at all, such as a stock CaffeineOS card:
   without it the AGA kernel cannot find the SD card. Running it again
   takes out whatever an earlier install added. SD-Setup does the same.
 * The installer and the uninstaller put the boot partition away
   safely as their last step, like a safe remove: everything is
   written out and the partition is gone until the power cycle. When
   the installer was started from that partition itself, it is only
   written out.
   Thanks to AngelClaww for both ideas.
 * The Pi 3 kernel was tested again on a Pi 3 A+ (AGA games, demos,
   sound over HDMI).
 * Tested on an A500 with a CM4 and with a Pi 3 A+; sound over HDMI not
   yet on a PiStorm16.

WHAT WAS NEW IN 1.111
---------------------
 * Not only CaffeineOS: the installer also sets up an AmigaOS 3.1.4 or
   3.2 of your own. It asks which one you have first. On AmigaOS 3.x it
   makes S:WHDLoad-Startup and S:WHDLoad-Cleanup if they are missing
   and switches them on in S:WHDLoad.prefs, takes over the kernel= line
   config.txt already has, and puts the Chipset menu in Workbench's own
   menu bar - and in a drawer, SYS:Chipset. WHDLoad and Picasso96 must
   be installed first. Tested on an A500 with AmigaOS 3.2.3 and
   WHDLoad 20. See THE BOOT PARTITION ON AMIGAOS 3.x if the installer
   cannot find your boot partition.
 * A1kmode, for an Amiga that cannot take a Framethrower (an A1000):
   Chipset menu -> Expert -> System wide -> "Keep it on (A1kmode)" keeps
   the whole machine on the emulated chipset at every boot, so every
   screen reaches HDMI. WHDLoad games still switch in and out by
   themselves. Hold the left mouse button while the Amiga starts to skip
   it once.
 * Floppy drives - DF0: or a Gotek - can be read while System wide or
   A1kmode is on. Writing to them is not tested yet.
 * Castlevania AGA draws its far background across the whole screen:
   AGA can repeat a sprite every 256 pixels ("SSCAN2"), and only the
   first copy was drawn. Thanks to gtv3000 for the report.
 * The archive unpacked on the Amiga (7z keeps no Amiga file bits) left
   the installer's own tools without the "executable" bit, which it
   reported as a missing boot partition. It now sets the bit itself,
   and says so plainly if its tool still cannot run.
 * The boot partition is found, and its config.txt changed, whatever
   case the card stores the names in (CONFIG.TXT).
   Thanks to AngelClaww for finding both.
 * The installer no longer touches the Pi's clock settings on a
   PiStorm16 (core_freq=500, which a classic PiStorm needs, troubled a
   PiStorm16, which wants 400).
 * The Chipset menu goes into every desktop menu the Amiga has:
   ToolsDaemon, Directory Opus 5 (also 5.102, the open-source Magellan
   II, which saves its user menu in a newer form), and Workbench's own
   menu bar unless ToolsDaemon has it. The installer's last window says
   where. A menu file it cannot write no longer stops the boot, and the
   S:User-Startup lines can no longer end the boot whatever happens.
 * Tested on an A500 with a CM4; not yet on a Pi 3 or a PiStorm16.

WHAT WAS NEW IN 1.110
---------------------
 * More in Chipset menu -> Output -> RTG Image (see PICTURE SIZE AND
   SHARPNESS below):
    - Flicker fixer: an interlaced screen is shown as one steady picture
      with all its lines, as a flicker fixer did on a real Amiga. On
      unless you turn it off.
    - Interlaced: scanlines for such a screen, set apart from Scanlines.
    - Brightness: 110% to 150%, to make up for the dark scanlines.
    - TV border: for a TV that cuts the edges off. The game picture, and
      the RTG desktop with its mouse pointer, are made 2% to 10% smaller.
    - Video: PAL or NTSC, the standard games start in. An NTSC game then
      runs at its own speed, with its music at the right pitch.
    - Settings for: all games, or the game you started last. A game can
      have its own picture, which it gets instead of the all-games one.
 * VideoCore.card is rebuilt for the TV border on the RTG desktop.
   Without a border it works as before.
 * The boot splash sits on black, whatever logo= says in cmdline.txt
   (CaffeineOS's logo=purple framed it in purple).
 * Everything else is as in 1.109. Tested on an A500 with a CM4 (RTG
   Image and games); not yet on a Pi 3 or a PiStorm16.

WHAT WAS NEW IN 1.109
---------------------
 * The installer finds the Emu68 boot partition on a card that
   CaffeineOS did not set up: config.txt and the Pi firmware on it are
   enough, it no longer needs a kernel drawer there (it makes one). If
   it still cannot find the partition, it asks you to pick it, and so
   does Uninstall. SD-Setup on the PC finds such cards too.
   The rest of the installer still expects CaffeineOS (see WHAT YOU
   NEED): it stops, changing nothing, if config.txt has no [gpio17=0]
   section or S:WHDLoad-Startup is missing.
 * A new boot splash.
 * Nothing else changed: games, HDMI and the RGB port are as in 1.108.

WHAT WAS NEW IN 1.108
---------------------
 * RGB port: how much of the picture it shows - Full frame 384 x 286 (as
   in 1.107), 368 x 280, 352 x 272 or the standard screen alone,
   320 x 256. The smaller ones are fewer bytes to send, so more frames a
   second, and they fit a monitor or scaler that cannot show the whole
   overscan. Chipset menu -> Output -> RGB port -> Size.
 * RGB port: Frame rate - Variable (a new picture as often as one gets
   through, up to 50, as in 1.107) or Fixed 25: every picture then stays
   up for two vertical blanks, so scrolling moves in even steps.
 * agaboot RUN <program> [arguments]: an AGA program that is not a
   WHDLoad game runs on the emulated chipset - it switches over before
   the program starts and back when it ends. See RUNNING ONE AGA PROGRAM
   below.
 * The ideas for these three come from Future Retro Fusion's
   PiStorm-AGA-HAM6 (see CREDITS AND LICENCES). Thank you!
 * Nothing else changed: games, HDMI and everything else are as in 1.107.
   Not tested on the A500 yet.

WHAT WAS NEW IN 1.107
---------------------
 * The Amiga's own RGB port can show the game too: Chipset menu ->
   Output -> RGB port. The Pi turns every frame into HAM6 - 4096
   colours - and puts it on the Amiga's own video output, the whole
   overscan area, where a real Amiga puts it. HAM6 and HDMI shows it on
   both; RGB port only keeps HDMI black. Position... moves it on your
   monitor. Off unless you turn it on. See THE AMIGA'S OWN RGB PORT
   below.
 * RTG Image is one window now: Size, Look, Scale, Scanlines and
   Position... (see PICTURE SIZE AND SHARPNESS below).
    - Look: TV blends neighbouring pixels across, as composite video did,
      for games dithered for a TV.
    - Scale: 4:3 (the shape a real monitor gives - a little wider than
      before), Integer (the sharpest) or Fill screen.
    - Scanlines: Off, 25%, 50%, 75% or 100% dark.
    - Position... puts a calibration frame on HDMI; the mouse or the
      cursor keys move the picture.
 * 640x256 now shows the standard screen where it really is. It sat
   eight lores pixels to the left and cut the right edge off. If you
   moved the picture right to make up for it, set X back to 0.
 * Virtual Karting 1 and 2 have their colours. Their "hardware chunky"
   super-hires mode now mixes each pair of pixels, as a monitor does
   (WinUAE's "Filtered low resolution"). With Look: TV the road and the
   grass come out the colours the game means. On Emu68 the game also needs
   WHDLoad's NoCache and ICNT=1 - in CaffeineOS the line
   VirtualKartingAGA;WNC ICNT=1 in its WHDLoad database.
 * The second and later games of a session are no longer sometimes
   refused ("requires AGA") or stopped halfway (since 1.101).
 * Roguecraft DX no longer flickers: the vertical blank interrupt now
   waits until the copper has set the screen up.
 * Code in chip RAM that a game rewrites now runs as rewritten; Emu68's
   code cache never saw those writes.
 * Raspberry Pi Zero 2 W: the installer sets it up like a Pi 3. Untested.
 * The WHDLoad hook now runs first in S:WHDLoad-Startup. Run the installer
   again, or agaboot INSTALL HOOKS, to move yours.
 * Licences: the kernel as a whole is GPL-2.0; the Emu68 source and its
   notices come with the package.
 * Tested on the A500 with a Pi 3 A+: the RGB port (Banshee, Synergy),
   Scale and Scanlines, Virtual Karting 1 and 2, Position. The Pi 4 / CM4
   and PiStorm16 kernels are built but not tested on hardware yet.

WHAT WAS NEW IN 1.106
---------------------
 * Blitter lines drawn with a pattern (dashed and dotted lines) start
   the pattern on the right pixel. Every dash used to be drawn one
   pixel early.
 * The AGA display window no longer drops 2048 lines down when a game
   sets the fine horizontal position bits in DIWHIGH. AGA reads only
   three vertical bits there; only an ECS Agnus reads a fourth.
 * The credits below, and every chipset source file, now say exactly
   what comes from UAE and WinUAE.
 * Not tested on the A500 yet: both fixes follow WinUAE.

WHAT WAS NEW IN 1.105
---------------------
 * OutRun AGA (reassembler's port, started from Workbench with System
   wide) shows the game. It used to show only the sky and the road
   colours - no cars, no scenery, no text. When a program switches the
   display off and on again, the copper now starts the program's own list
   the way a real Amiga does, instead of an old one from the system.
 * Shadow Fighter AGA no longer freezes after its title screen. A timer
   interrupt the game never answers is now cleared, where it used to
   keep the game stuck in its interrupt handler.
 * Chipset registers set many at a time with one instruction (MOVEM) no
   longer lose everything past the first eight bytes.
 * PiStorm16: after Ctrl-Amiga-Amiga the session is kept for
   C:AGA-diag.log and agastat PREV, as on the classic PiStorm.
 * Tested on the A500: OutRun AGA (menu and racing), Shadow Fighter AGA,
   Chaos Engine AGA and Turrican 2 AGA.

WHAT WAS NEW IN 1.104
---------------------
 * Faster. The chipset does the same work in about a third less time,
   and on the A500 test machine frames that ran late (Reshoot Proxima 3's
   busy levels choked and its sound stuttered) went from 39% to none at
   all. The picture is exactly the same as before.
 * Banshee no longer gets black rectangles over the sea. 1.103 brought
   them in with the Synergy fix; that part is undone, and Synergy still
   plays to the end.
 * Nexus 7's Gouraud scene stays clean with the default settings.
 * Sound: a game writing a sample word to a quiet channel while that
   channel still has its interrupt pending is ignored, as on a real
   Amiga (and in WinUAE).
 * Tested on the A500: Banshee, Nexus 7 (start to finish), Reshoot
   Proxima 3 (start to finish), Alien Breed, Worms: The Director's Cut
   and Synergy.

WHAT WAS NEW IN 1.103
---------------------
 * Chipset menu -> Output -> RTG Image: how much of the game picture
   fills your screen (Full, 720x272 or 640x256) and whether the pixels
   are Sharp or Smooth. Remembered for good. See PICTURE SIZE AND
   SHARPNESS below.
 * The Chipset menu is tidier: AGA toggle, RTG Image, System wide, and
   About at the bottom.
 * Demos that draw while the picture is being shown ("racing the beam")
   look right: Nexus 7's Gouraud scene had a hard line through it with
   the old object below it, like a see-through box. The raster position
   a program reads could run far ahead of what was really drawn.
 * Demos that chain their blits from the blitter interrupt no longer
   hang when the machine is busy - Synergy could stop at its last scene
   with the music still playing.
 * A game or demo that hangs now logs itself: SetEnv SAVE AGA_DIAGLOG 1
   once, and after F10 - or after the Ctrl-Amiga-Amiga that gets you out
   of a hang - everything is in C:AGA-diag.log. Send that file with a
   bug report.
 * Tested on the A500: Worms: The Director's Cut, Banshee, Alien Breed:
   Tower Assault, The Naughty Ones, Roadkill, Reshoot Proxima 3, and the
   demos Synergy, Shiva, Nexus 7 and Starstruck (the 060 monster, start
   to finish). Demos that start from their own icon run on the Amiga's
   own chipset and show nothing or only play music - start them through
   WHDLoad (a slave) and they get the AGA chipset.

WHAT WAS NEW IN 1.102
---------------------
 * Worms: The Director's Cut plays. Its menu used to freeze with the
   music stuck on one note: when a game switches a sound channel off,
   real Paula plays out the word it has and asks for another, and Worms
   waits for exactly that request. The emulated one went silent
   instead. Menu, game start, the game itself and F10 back to the
   desktop all work now.
 * Sound effects that were cut off after a split second play in full -
   Banshee's explosions and bonus sounds were the ones you noticed. The
   channel's "sample started" interrupt came a whole sample period late,
   and the game took it for "sample finished".
 * The blitter now takes about as long as a real one before it says it
   is done (the picture data is still ready at once). Demos that chain
   their blits from the blitter interrupt - Synergy by Sector 7 is one -
   could lose a link of the chain and wait for ever while the music
   played on: whole effects never appeared.
 * AGA scan-doubling (BSCAN2) is emulated: a screen that shows every line
   twice from half the memory no longer comes out half height with
   rubbish underneath.
 * A demo that has taken over the keyboard can still be left: hold both
   mouse buttons for two seconds, or the left one for six, and WHDLoad
   gets its F10.
 * For bug hunting: after a keyboard reset the kernel keeps the log of
   the session that hung - `agastat PREV` shows it, `agastat PREVSNAP`
   saves the screen as the reset found it, `agastat PREVPROF` where the
   CPU spent the last 80 seconds.
 * The boot picture fills about three quarters of the screen instead of
   a tenth of it.
 * Tested on the A500: Worms: The Director's Cut, Banshee, Synergy, one
   after the other, each back to the desktop with F10.

WHAT WAS NEW IN 1.101
---------------------
 * Games that showed nothing but a black screen now run. A game is
   allowed to open its display window on the very first line of the
   frame, inside the vertical blank; I was throwing that away, so the
   window never opened and not one pixel was ever fetched - eight
   bitplanes set up, DMA on, pointers into real artwork, and a black
   screen for ever. Worms: The Director's Cut was the game that showed
   it; anything whose display starts that high was hit the same way.
 * PAL games that briefly write BEAMCON0 twice in one frame no longer
   end up in NTSC. The frame only changes size at the next frame now,
   which is what the real Agnus does - in between, a 313 line game was
   waiting for raster lines that a 263 line frame never reaches.
 * A game that locks up hands the machine back instead of taking it with
   it: if the 68k stops talking to the chipset for ten seconds, the
   sandbox is released and the desktop returns, so you can save your
   work and quit properly rather than pulling the plug.
 * Keyboard and mouse interrupts now reach a game that has gone to sleep
   waiting for one. (`aga.cia=0` in EMU68:boot/cmdline.txt turns this
   off, `aga.cia=2` extends it to the CIA-B timers.)
 * New boot picture, and the Chipset menu has a "System wide" entry -
   the whole machine on the emulated chipset, desktop and all. It is
   UNSTABLE and NOT RECOMMENDED: it is there because people run
   `agaboot SANDBOX ON` from a Shell anyway and report that most things
   work for them. Games do not need it.
 * Worms: The Director's Cut still stopped at its menu in 1.101 - fixed
   in 1.102, see above.

WHAT WAS NEW IN 1.1
-------------------
 * Raspberry Pi 3 support, with a kernel of its own. The installer finds
   out which Pi you have (or asks) and puts the matching kernel on the
   card. On a Pi 3 the picture skips a frame now and then when a game
   gets busy, so the music stays right. Tested with Banshee AGA.
 * 1 MB of Chip RAM is enough - an A500 or A500+ without a trapdoor card.
 * Faster AGA picture on the Pi 4 / CM4: each frame is drawn straight
   into the Pi's display memory instead of being copied there.
 * Chipset menu -> About: who made it, and the licences.
 * Swapped the Pi? Chipset menu -> Init tells you when the kernel on the
   card was installed for the other one - then just run the installer
   again.

HOW IT STARTED
--------------
Started as a birthday gift for my big brother (he noticed i was into amiga and wanted to be a part of it, he's also a hobby developer so thats a big win in my book since i'll potentially get help now with my hobby ports!) i couldn't give him my A1200, but i had A500+ which i used from time to time. Still thought came to me that with A500 he will not be able to experience all of the games on the Amiga (i.e. AGA games) i then decided to just do this (strange noone else did this SEE=> IF SOMETHING GOES WRONG). So long story short next chapter it is. 

WHAT IT IS (PLEASE MAKE A BACKUP OF YOUR EMU68 SDCARD)
------------------------------------------------------
In short: AGA-PISTORM lets you load AGA WHDLoad games - the ones made for the
A1200 and A4000 - now run on OCS/ECS Amiga (A500 (1 MB of chip is enough), A500+,maybe A600,maybe A2000)
fitted with a classic PiStorm (tested on one w EPM240 CPLD) and a Raspberry Pi 4 or a Pi 3.

The Pi emulates a complete AGA chipset (Alice, Lisa and Paula) on
its core 3 or fourth core. While a game runs, it runs on that chipset: AGA graphics,
music and sound effects. When you quit the game, your Amiga is back on
its own chipset and you can switch live anytime you want. Not everything is emulated tho!
This is made possible by PI2D(cracked the oracle) driver that i made awhile back for youtube direct play (works)
and wanted to actually watch directly youtube on my amiga, also it was for unnamed port for which i'll
not disclose the name and i stopped working on it since then (SDL2(threading)+2D driver gives you playable exp).

Played so far with this: Banshee AGA, Pinball Fantasies AGA, Aladdin AGA,
Turrican AGA, ClockwiserAGA, Reshoot proxima3, Virtual Karting 1 and 2, and
the demos Synergy, Nexus 7, Shiva and Starstruck and more - several in a row, without
rebooting
=> If a game doesnt work reboot and try again. 
There might be some graphical glitches and bugs!


WHAT YOU NEED
-------------
 * A classic PiStorm (the CPLD one for the A500, A500+, A600, A2000).
 * A Raspberry Pi 4 (or a Pi 400 / Compute Module 4).
   Or a Raspberry Pi 3, on a classic PiStorm: the installer asks which
   Pi you have and puts the Pi 3 kernel on the card. Swap the Pi later?
   Run the installer again - Chipset menu -> Init reminds you.
   A Raspberry Pi Zero 2 W is the Pi 3's chip family and takes the Pi 3
   kernel too. Nobody has tried one yet - tell me how it goes.
   A Pi 4 on a classic PiStorm needs the EPM240_longer_hold CPLD
   firmware - if your Pi 4 already runs CaffeineOS, you have it.
   The firmware files are a separate download (CPLD) on the itch.io
   page.
 * 1 MB of Chip RAM is enough. The games run in 2 MB of the Pi's own
   memory whatever the Amiga has; while a game runs, the missing
   megabyte is lent to the OS and taken back afterwards. Tested on an
   A500+ with its trapdoor card removed: ECS games, Turrican II AGA
   and Banshee (2 MB, KickEmu) all run. 512 KB machines are untested.
 * CaffeineOS with its WHDLoad setup, and its RTG (Picasso96) desktop.
   Tested with CaffeineOS 9317 and WHDLoad 20.
   Or AmigaOS 3.1.4 / 3.2 with WHDLoad and Picasso96 installed, and the
   Emu68 boot partition mounted (see THE BOOT PARTITION ON AMIGAOS 3.x).
   Tested with AmigaOS 3.2.3 and WHDLoad 20. Two things the installer
   cannot do for you there:
     - A VideoCore monitor in DEVS:Monitors (CaffeineOS has one).
       Picasso96 starts VideoCore.card only through a monitor icon whose
       BOARDTYPE tooltype says VideoCore - without one there is no RTG
       desktop on HDMI. If Picasso96's installer offers no VideoCore
       board, let it install any other one (PicassoIV does nicely). Then
       select that board's monitor in DEVS:Monitors, choose Copy in
       Workbench's Icons menu, rename the copy VideoCore, and in Icons ->
       Information set its tooltype BOARDTYPE=VideoCore; leave its other
       tooltypes alone. Forgot? The installer tells you at the end when
       there is no such icon (it installs all the same) - make it then
       and power cycle.
     - VideoCore screen modes, after installing and the power cycle. If
       Prefs/ScreenMode lists none, make them in Prefs/Picasso96Mode: a
       setting for the VideoCore board, a resolution, and its colour
       depths. Then pick one in ScreenMode, Test it, and Save.
 * For the games that need one, the WHDLoad Kickstart file in
   DEVS:Kickstarts (Banshee, for example, needs kick40068.A1200).
   CaffeineOS comes with them.
 * If you like: a monitor on the Amiga's own RGB port, to see the games
   there too (Chipset menu -> Output -> RGB port). HDMI is all you need
   otherwise.

Emu68: you need Emu68 1.1.0-beta.1/CaffeineOS 9317 stock preferrably already installed, everything else will be installed.


WHAT IS IN THE PACKAGE
----------------------
 README.txt                 this file
 SD-Setup.cmd               the one-time step on a Windows PC
 PC\SD-Setup.ps1            what SD-Setup.cmd runs
 Amiga\AGA-Pistorm\         everything for the Amiga:
   Install                  the installer - double-click it
   Uninstall                puts everything back the way it was
   ReadMe.txt               this file again
   Files\Kernel\Emu68-classic-aga.gz
                            the AGA-PISTORM kernel: Emu68 1.1.0-beta.1
                            (git 0ba3a89) with the AGA chipset, for the
                            classic PiStorm and a Pi 4 / Pi 400 / CM4
   Files\Kernel\Emu68-classic-aga-pi3.gz
                            the same for a Raspberry Pi 3 or Zero 2 W - the
                            installer puts whichever matches your Pi on the card
   Files\Kernel\Emu68-pistorm16-aga.gz
                            the same for the PiStorm16 (see PiStorm16.txt)
   Files\Libs\Picasso96\VideoCore.card
                            VideoCore 1.5, the Pi's RTG (2D) driver, with
                            my changes from the "PI Mailbox GPU driver"
                            project, and the TV border (1.110). The
                            unmodified 1.5 does NOT work with AGA-PISTORM -
                            the AGA picture freezes.
   Files\Overlays\          Emu68 1.1 overlays: sdhc, emmc, unicam, pal,
                            ntsc and the rest, with their documentation
   Files\C\agaboot          switches games onto the AGA chipset and back,
                            the Chipset menu (Init, Output, Sound, Expert,
                            About), and the installer's helper
   Files\C\agastat          diagnostics - you will not need it
 Amiga\AGA-Pistorm.info     the drawer's icon
 Licenses\                  the licence texts 
 Source\                    the source code 


INSTALLING
----------
STEP 1 - on a PC (one time)

  1. Unpack the whole archive into a folder on the PC, with its folders
     as they are. SD-Setup.cmd runs PC\SD-Setup.ps1 beside it, and that
     takes the drawer from the Amiga folder beside that: started from
     inside an archive window, or copied out on its own, it finds
     neither, and stops without changing anything on the card.
  2. Switch the Amiga off and put its SD card in the PC. If Windows
     offers to format a drive, click Cancel! That is the Amiga's own
     partition, which Windows cannot read - formatting it erases
     everything your Amiga keeps there.
  3. Double-click SD-Setup.cmd. It finds the card's Emu68 boot partition
     (whatever drive letter Windows gave it) and asks before it changes
     anything. Then it:
       - backs up config.txt as config.txt.pre-aga,
       - lets the Amiga write to that partition (Emu68 1.1 keeps it
         read-only otherwise),
       - adds the Emu68 overlays the card is missing,
       - copies the AGA-Pistorm drawer onto the card.
  4. Eject the card safely and put it back in the Amiga.

  No Windows PC? Copy the Amiga\AGA-Pistorm drawer and AGA-Pistorm.info
  onto the card's boot partition any way you like and carry on with
  step 2. The installer tells you if it needs the PC step.

STEP 2 - on the Amiga

  1. Switch on. Open the boot partition's disk icon on CaffeineOS
     (mount EMU68) then the AGA-Pistorm drawer. On AmigaOS 3.x, open
     the drawer wherever you put it.
  2. Double-click Install. It checks your Amiga and asks a few questions:
       - CaffeineOS, AmigaOS 3.x or AmiKit?  (see AMIKIT AND MULTIBOOTOS)
       - Where is the boot partition?  (only when it cannot find it)
       - Which Raspberry Pi?           (only when it cannot tell)
       - Do you have a Framethrower?   (Yes adds the unicam overlay)
       - Is your Agnus PAL or NTSC?    (NTSC adds the pal overlay, so the
                                        system runs as PAL like the games
                                        expect not as Jesus intended :) )
       - Run the SD card at 25 MHz?    (Pi 4 only, for cards that lose a
                                        write now and then; No keeps 50)
       - Games on the RGB port too?    (HAM6 on the Amiga's own monitor,
                                        with HDMI or alone; No keeps it
                                        dark. Chipset menu - Output -
                                        RGB port changes it later)
       - Which rate for HDMI?          (1080p at 50 Hz, the recommended;
                                        50.2 Hz, room for HDMI sync; 60
                                        Hz; or the display's own setting)
       - A PC monitor or a TV?         (full range colours for a monitor,
                                        a TV's limited range for a TV)
       - Sound on HDMI as well?        (for the display's speakers or an
                                        HDMI audio extractor)
     Then it installs the kernel (read back after copying, so a card
     that loses a write gets no broken one) and its config.txt line, the
     overlays, VideoCore 1.5, the WHDLoad hooks and the Chipset menu. On
     CaffeineOS the Caffeine button's start menu gets an AGA-PISTORM
     sub-menu too, above Karma, with a chip for its icon (Init, Output,
     Sound, Monitor, Expert, About). Everything it changes is backed up
     first as a .pre-aga file.
  3. When it tells you to, POWER CYCLE the Amiga: switch it off, wait a
     few seconds and switch it on. A Ctrl-Amiga-Amiga reset is not
     enough - the new kernel only loads at power-on.
  4. Did it work? In a Shell, type  C:agaboot CHECK  and look for
         AGA-PISTORM kernel       for a Pi 4, running on a Pi 4
     (a Pi 3 on a Pi 3). "- run the installer again" after it means the
     card moved to the other Pi since. "NOT running" there means the
     Amiga was reset instead of power cycled, or config.txt starts
     another kernel. The same list also says whether the boot partition
     is writable and whether DEVS:Monitors has a VideoCore monitor icon.
  5. You can add your PI4 config.txt settings later whatever you used before

THE BOOT PARTITION ON AMIGAOS 3.x
---------------------------------
The installer needs the Emu68 boot partition - the FAT one with
config.txt on it - mounted, as CaffeineOS mounts it as EMU68:. Many
AmigaOS 3.x setups do not mount it at all. Then mount it with fat95:
a file like this in SYS:Storage/DOSDrivers, and Mount it when needed:

    FileSystem     = L:fat95
    Device         = brcm-emmc.device   /* a Pi 3: brcm-sdhc.device */
    Unit           = 0
    Surfaces       = 1
    BlocksPerTrack = 1
    BlockSize      = 512
    Reserved       = 0
    LowCyl         = 2048      /* the partition's first sector */
    HighCyl        = 264191    /* ...and its last one */
    Buffers        = 40
    MaxTransfer    = 0x0001FE00
    Mask           = 0x7FFFFFFC
    StackSize      = 16384
    GlobVec        = -1
    DosType        = 0x46415401

Unit 0 is the whole card - read-only unless config.txt has
dtoverlay=emmc,unit0=rw (a Pi 3: dtoverlay=sdhc,unit0=rw); with an older
Emu68 1.0.x, unless cmdline.txt has emmc.unit0=rw (a Pi 3: sd.unit0=rw) -
so LowCyl and HighCyl are sectors: take them from the card's partition
table (on a PC, for example). The numbers above are one card's - a 128 MB partition
starting at sector 2048.

Some cards come with an icon that mounts it for you instead - MOUNTFAT,
for one. Once it is mounted, the installer finds it by itself; when it
asks, click Volumes in its requester to see the partition in the list.

AMIKIT AND MULTIBOOTOS
----------------------
MultibootOS
Run Install once from every system on the card that you want AGA on.
MultibootOS keeps a config file for each system (config_<os>.txt) and
copies it over config.txt whenever you pick that system in its boot
menu. The installer knows about this: it changes the matching
config_<os>.txt together with config.txt and tells you which one it
was. Uninstall does the same.

AGA-PISTORM goes into one system at a time: pick the system in the boot
menu, run Install there, power cycle - then the next one, if you want it
there too. Install names the systems that still boot their own kernel,
and so does C:agaboot CHECK. Don't copy AGA-PISTORM's files into another
system by hand: they run there without our kernel.

MultibootOS's AmigaOS 3.2 comes without the Installer program. Open a
Shell there and type this first (AGS's partition is mounted as AGS0:):
    Copy AGS0:C/Installer SYS:C/
then double-click Install as usual.

AmiKit
When Install asks which system you have, pick AmiKit - it's already
selected when the installer recognises AmiKit. You get everything you
would get on AmigaOS 3.x, plus the Chipset menu in Directory Opus. A few
things are different on AmiKit, though:

  - WHDLoad gets a little helper. On AmiKit, every AGA game said "This
    game requires AGA chipset" even with Init on: WHDLoad runs its
    startup script in the background and doesn't wait for it, so the
    emulated chipset came up too late. So the installer puts a small
    stand-in in C:WHDLoad and renames the real one to C:WHDLoad.real.
    The stand-in switches the emulated chipset on first, then starts the
    real WHDLoad with exactly the same arguments, and switches the
    chipset off again when you quit the game. (Version C:WHDLoad shows
    it as WHDLoad-AGA.)

  - IF YOU UPDATE WHDLOAD - with AmiKit's LiveUpdate, WHDLoad's own
    installer, or a new C:WHDLoad copied by hand - the new WHDLoad
    replaces the stand-in, and AGA games say "This game requires AGA
    chipset" again. Nothing is broken: just run Install again and pick
    AmiKit. It keeps your new WHDLoad as C:WHDLoad.real and puts the
    stand-in back. Nothing else changes.

  - AmiKit comes with VideoCore 1.9 (RGB2RTG's build), a higher number
    than this package's 1.5. The installer puts ours in all the same - a
    driver made for another kernel may not know this one - and keeps
    AmiKit's as Emu68-VC4.card.pre-aga (or .replaced, if an earlier
    install kept one already). Uninstall puts the .pre-aga one back.

  - AmiKit's own S:WHDLoad-Startup slows Emu68 down for every game
    (SYS:System/EmuControl ICNT=2 nocache). That's AmiKit's setting, not
    ours, so games may run a little slower there than on other systems.

  - AmiKit's WHDLoad doesn't preload games (its S:WHDLoad.prefs has no
    Preload line; CaffeineOS's does). A game that loads its files while
    you play then holds up the emulated chipset at every load, and on a
    Pi 3 you can hear the sound crackle - Reshoot Proxima 3 does it at
    its intro and when a level starts. Easy to fix: give the game's icon
    the tool type PRELOAD, or add a line Preload to S:WHDLoad.prefs for
    all games. Starting a game from a Shell? Tool types don't count
    there - add Preload to the command line.

Uninstall puts the real WHDLoad back as C:WHDLoad.


USING IT (DOES NOT WORK WITH ADF'S!)
------------------------------------
AGA is OFF after installing. To switch it on:

      Chipset menu  ->  Init  ->  Automatic

From then on every WHDLoad game you start runs on the emulated AGA
chipset, and when you quit it (F10) your Amiga is back on its own
chipset. You can play one game after another without rebooting.

Click Init again and choose Turn off to go back to the native chipset:
games then run exactly as they did before. The setting is remembered
when you switch off.

The Chipset menu has five items: Init, Output, Sound, Expert and About.

Chipset menu  ->  Output  ->  RTG Image  sets how big and how sharp the
game picture is (see PICTURE SIZE AND SHARPNESS below).

Chipset menu  ->  Output  ->  RGB port  puts the game on the Amiga's own
RGB port too, as HAM6 (see THE AMIGA'S OWN RGB PORT below). Off unless
you turn it on.

Chipset menu  ->  Sound  sends the sound over HDMI too (see SOUND OVER
HDMI below).

Chipset menu  ->  Expert  ->  System wide  puts the whole machine on the
emulated chipset, and  Expert  ->  Run program...  one program (see
RUNNING ONE AGA PROGRAM below). WHDLoad games need neither, and either
can freeze the machine, so save your work first.

Chipset menu  ->  About  shows who made AGA-PISTORM and the licences.

On AmigaOS 3.x the Chipset menu is in Workbench's menu bar, next to
Tools (hold the right mouse button), and the same items are in the
drawer SYS:Chipset.


PICTURE SIZE AND SHARPNESS
--------------------------
The emulated chipset draws the whole overscan area, borders included, and
by default all of it is fitted to your screen. A normal 640x256 screen
(the Workbench, most games) then sits in the middle with a wide black
border. Chipset menu -> Output -> RTG Image sets the picture:

   Size        Full frame 800 x 286  the whole overscan area (the default)
               720 x 272             what a TV shows - a little of the border
               640 x 256             the standard screen
               Custom                the frame less the edges you cut off
                                     with Crop... (below)
               Auto                  the game's own display window,
                                     followed as it plays: with Scale
                                     Fill screen the game fills the display
   Look        Sharp                 crisp square pixels, all the same
                                     width also where the picture is
                                     enlarged by no whole number
               Smooth                the scaler's soft edges (the default)
               TV                    neighbouring pixels blended across, as
                                     composite video did - for games
                                     dithered for a TV, like Virtual Karting
   Scale       4:3                   the shape a real monitor gives (the
                                     default)
               Integer               a whole number of HDMI lines for every
                                     Amiga line: the sharpest
               Fill screen           stretched over the whole display
   Scanlines   Off, 1% ... 100%      dark lines between the Amiga's lines, as
                                     on a CRT - a slider
   Interlaced  Off, 1% ... 100%      the same for an interlaced screen that
                                     the flicker fixer shows
   Scanlines (NTSC)
               As PAL (the default)  NTSC screens' own strength: a CRT shows
               Off, 1% ... 100%      NTSC's lines stronger (a 1084S about
                                     40% dark against PAL's 27%). A game
                                     that switches standard gets the other
                                     lines from that frame on.
   Flicker fixer
               On (the default)      an interlaced screen as one steady
                                     picture with all its lines, when HDMI
                                     has room for them (720p and up)
               Off                   each half-picture as it comes: the
                                     screen flickers, as before 1.110
   Scanline shape
               Even (the default)    every Amiga line as dark, also with 4:3
               Crisp                 one whole dark HDMI line a line
               Whole lines           a whole number of HDMI lines a line, the
                                     picture a little smaller
               Whole lines (fill)    the next whole number up, the picture
                                     full height and the border's edges cut
               Beam, Beam (fill)     the two above with each line darker
                                     towards its edges, as a CRT's beam
   Scanline position
               Bottom (the default)  the dark line at each line's bottom
               Top                   at its top
               CRT fields            Bottom, and with Flicker fixer Off an
                                     interlaced screen's every other field
                                     half a line lower, as a CRT draws it
   CRT mask    Off, 25% ... 100%     an aperture grille's dark columns
   Brightness  Normal ... 150%       110%, 120%, 135% or 150%: makes up for
                                     the scanlines' dark lines. The RGB port
                                     stays as it was.
   Gamma       Exact (the default)   the Amiga's colours; CRT, a 1084S's
                                     darker mid-tones; Lighter
   Frame blend Off (the default)     each frame mixed with the one before:
               On                    games' 50 Hz flicker and every-other-
                                     frame scrolling look steady
   Curved screen, Glow, Afterglow (a Pi 4 or CM4 only)
               Off (the default)     drawn by the Pi 4's 3D GPU: the glass
                                     bent (1-9), the light around bright
                                     parts (1-100%), a phosphor's trail
                                     (Light, Medium, Strong). Not with Look
                                     TV: the GPU draws Sharp and Smooth
   Keep after power cycle
               No (the default)      Frame blend, Curved screen, Glow and
                                     Afterglow off again after the next
                                     power cycle, even with Save
               Yes                   kept like the other rows
   1084S look  sets the CRT look as a Commodore 1084S shows the Amiga:
               scanlines 27%, 40% on NTSC screens (none on interlaced ones -
               their fields fill the gaps), CRT mask 25%, Look Smooth, Gamma
               CRT, Brightness 120%. A start to change from.
   No CRT look scanlines, mask, Brightness, Gamma and the GPU's effects
               back to none
   Default     every row of Picture and CRT look as a new install has them;
               Custom's Crop edges and This Amiga's rows stay
   Save as Custom look
               keeps Look and the CRT look's rows as they are now as your
               own - at once; Cancel does not take it back
   Custom look puts your own back, for all games or the game in Settings
               for (greyed until you have saved one)
   TV border   Off, 2% ... 10%       the picture that much smaller, centred,
                                     for a TV that cuts the edges off. The
                                     all-games border shrinks the RTG
                                     desktop and its mouse pointer too.
   Video       PAL or NTSC           the standard games start in, from the
                                     next game on: NTSC for a game made for
                                     60 Hz. A game that sets its standard
                                     itself still gets its own.
   HDMI rate   As config.txt says    (the default)
               50 Hz (1080p50)       config.txt's HDMI mode set to 1080p
               60 Hz (1080p60)       at 50 or 60 Hz, from the next power
                                     cycle - for HDMI sync
               50.2 Hz (for sync)    1080p50 a little faster: room for
                                     HDMI sync to follow games and demos
                                     whose frames are a line short (50.08
                                     Hz), which 50 Hz cannot. PC monitors
                                     take it. If a TV shows no picture: on
                                     a PC, delete the four hdmi_ lines in
                                     config.txt's AGA-PISTORM block
   HDMI colours
               PC monitor            the default: full range, black at 0,
                                     white at 255
               TV                    limited range, black at 16, white at
                                     235, as a TV
                                     expects in 1080p50 - from the next
                                     power cycle, like HDMI rate
   HDMI sync   Off                   (the default)
               Follow the Amiga      HDMI's frame made a line or two longer
                                     to keep pace with the game: on a 50 Hz
                                     screen a PAL game (on 60 Hz an NTSC
                                     one) shows every frame exactly once,
                                     so scrolling is smooth
   Position... puts a calibration frame on HDMI - the border of a standard
               screen - and the mouse or the cursor keys move the picture
   Crop...     the edges for Size Custom: HDMI shows the last game's
               picture cut to them, each edge's number by it (with no game
               since the Pi started, the calibration frame). Tab picks an
               edge, the cursor keys, W A S D or a drag move it (Shift:
               bigger steps), N switches between the PAL and the NTSC
               edges, 0 starts again from 720 x 272; the right mouse button
               or Return keeps them, Esc puts them back

Every change shows at once. Save keeps it for good, Use until you switch
off. HDMI rate and HDMI sync are for every game; HDMI rate needs the
boot partition mounted, as Sound does (see SOUND OVER HDMI). Or type in a
Shell, any of the words in any order:

   agaboot ZOOM 640x256 SHARP INTEGER SCANLINES=2
   agaboot ZOOM 720x272 TV X=4 Y=-2
   agaboot ZOOM LACED=1 BRIGHTNESS=2 BORDER=4 NTSC

The words: FULL, 720x272, 640x256, CUSTOM or AUTO; SHARP, SMOOTH or TV;
4:3, INTEGER or FILL; SCANLINES=0-4 and LACED=0-4 (Off, 25% ... 100%),
NTSCLINES=0-4 (NTSCLINES=PAL: as SCANLINES);
SCANPCT=, LACEDPCT= and NTSCPCT= the same in percent (0-100);
SCANSHAPE=EVEN, CRISP, WHOLE, WHOLEFILL, BEAM or BEAMFILL; SCANPOS=BOTTOM,
TOP or FIELDS; MASK=0-4; GAMMA=EXACT, CRT or LIGHTER; FRAMEBLEND=ON or OFF;
CURVE=0-9, GLOW=0-100 and AFTERGLOW=OFF, LIGHT, MEDIUM or STRONG (a Pi 4);
KEEPFX=ON or OFF (Keep after power cycle);
FLICKERFIX=ON or OFF; BRIGHTNESS=0-4 (Normal, 110%, 120%, 135%, 150%);
BORDER=0-10 (in %), or BORDERX= and BORDERY= for across or down alone; PAL
or NTSC; X= and Y=; and for CUSTOM, CUT=left,top,right,bottom (PAL) and
NTSCCUT= (NTSC), in hires pixels and lines; CRTLOOK=1084S, NONE or CUSTOM
(the buttons above), CRTLOOK=SAVE keeps the look as your Custom look.
agaboot CROP is Crop.... Help in the window scrolls; the names in it are
in bold.

The setting lives in ENVARC:AGA_ZOOM; delete it (and ENV:AGA_ZOOM) to go
back to the default. Games that draw into the borders lose their edges at
640x256; 720x272 keeps most of them. Without AGA_ZOOM, games get FULL and
the system wide mode gets 640x256. Both also take the setting directly:

   agaboot SANDBOX ON 640x256 SHARP
   agaboot SYSTEMWIDE ASK 720x272

A game can have a picture of its own. Start the game once and quit it;
RTG Image's "Settings for" then offers it next to All games and demos -
or Add... picks any game's or demo's drawer. Pick it, set its picture and
Save: from then on that game gets its own, and every other game the
all-games one. A demo or program started with Run program... gets one by
its drawer the same way. Remove gives it the all-games one again. In a
Shell, GAME= comes first, with the name of the game's drawer (GetEnv
AGA_LASTGAME shows the one started last):

   agaboot ZOOM GAME=<drawer> BRIGHTNESS=2 NTSC
   agaboot ZOOM GAME=<drawer> REMOVE

Each game's own picture lives in ENVARC:AGA-PISTORM/Games/.


THE AMIGA'S OWN RGB PORT
------------------------
While a game runs on the emulated chipset, the Amiga's own video output
normally stays dark: the picture goes out on HDMI. Chipset menu ->
Output -> RGB port puts it on the Amiga's RGB port as well:

   Picture     Off                   the RGB port stays dark (the default)
               HAM6 and HDMI         the game on both
               RGB port only         the game on the RGB port, HDMI black
   Palette     HAM6 (4096 colours)   the default
               32 colours            smoother motion: fewer bytes for the
                                     Amiga's bus, but gradients show bands
   Size        Full frame 384 x 286  the whole overscan area (the default)
               368 x 280, 352 x 272  less of the border
               Standard 320 x 256    the standard screen alone
   Frame rate  Variable, up to 50    a new picture as often as one gets
                                     through
               Fixed 25              a steady 25: every picture stays up
                                     for two vertical blanks
               Auto                  50 while 50 gets through, a steady 25
                                     while it does not (the default)
   Colours     Exact (rounded)       each colour to the nearest one HAM6
                                     has (the default)
               Smooth (dithered)     a fine pattern of the two nearest:
                                     smoother gradients, a little grain,
                                     a few per cent fewer frames
   Position... moves the picture on your RGB monitor

The Amiga's own chipset cannot show 256 colours, so the Pi turns every
frame into HAM6 - 4096 colours, in the Amiga's lores - and sends only what
changed since the frame before. It shows the whole overscan area, placed
where a real Amiga puts it, so a CRT shows it the way it would show a real
A1200. It never tears: every frame is finished before it is shown. HDMI
keeps its full 50 either way.

How many frames a second get through depends on how much of the picture
changes. The Pi writes each change over the PiStorm, one word at a time,
about 1.3 microseconds a word, so a frame can change some 15,000 words and
still make the next vertical blank. A picture that only moved up or down
costs almost nothing: the Amiga's display is pointed at it where it now
is, and only the new rows are sent - pinball tables and vertical shooters
run at 50. On a Pi 3 A+, Banshee AGA, The Chaos Engine AGA and Pinball
Fantasies AGA ran at 50; games that scroll sideways, like Jim Power or
Brian the Lion, change up to twice the 15,000 words while they scroll,
and Auto holds them at a steady 25 rather than an uneven 30. Palette 32
colours sends about a fifth fewer words (and has no HAM fringes), which
helps a game whose busiest moments are only a little too much for HAM6 -
at the cost of colours. A smaller Size means fewer words too. While a
game runs it borrows 162 KB of chip RAM, and gives it back when you quit.

Position... shows a calibration frame on the RGB monitor - the border of a
standard screen, with the offsets in the middle. The cursor keys, W A S D
or the keypad move it (Shift: eight at a time); Return keeps it, Esc puts
it back; then Save. Every game lands where the frame was. Nothing can go
above the top of the Amiga's own picture, so if an LCD or a scaler cuts
the bottom off, its own vertical position setting is the better fix.

From a Shell:

   agaboot RGBPORT HAM6               on, HDMI too
   agaboot RGBPORT ONLY               on, HDMI black
   agaboot RGBPORT OFF                off
   agaboot RGBPORT HAM6 X=-6 Y=-10    on, moved 6 left and 10 up
   agaboot RGBPORT 320x256 FIXED25    the standard screen, a steady 25
                                      (384x286 368x280 352x272 320x256,
                                      VARIABLE, FIXED25 or AUTO)
   agaboot RGBPORT 32COLOURS          Palette: 32 colours (4096COLOURS:
                                      HAM6)
   agaboot RGBPORT SMOOTH             Colours: Smooth (EXACT: Exact)
   agaboot RGBPORT POSITION           the calibration frame

The setting lives in ENVARC:AGA_RGBPORT. The Pi's second core draws it,
which it cannot do if Emu68 runs with async_log. Not tested on a
PiStorm16 yet.


SOUND OVER HDMI
---------------
The emulated chipset's sound - a game's, a demo's, the whole Workbench's
in System wide - always comes out of the Amiga's own audio output.
Chipset menu -> Sound -> Settings... can send it over HDMI as well, so
the TV, monitor or AV receiver plays it (or an HDMI audio extractor
passes it on). Sound's other items - Monitor..., Equalizer... and
Voices... - are further down. The Settings window:

   Sound on HDMI     Off             the Amiga's own output only (the
                                     default)
                     On              the sound on HDMI too
   HiFi sound (AHI)  Off / On        AHI programs straight on HDMI, in up
                                     to 24 bits (see HIFI SOUND below);
                                     Off is the default
   Paula on HDMI     Classic         the sound as before (the default)
                     Band-limited    built from Paula's exact steps: no
                                     aliasing, cleaner high notes
   Sound rate (HDMI) Follow screen   an AHI program's rate, when the
                                     screen says it takes it (the default)
                     Follow, any     an AHI program's rate, whatever the
                                     screen says
                     96 kHz, 192 kHz HDMI held at that rate, for
                                     everything
   Filter (HDMI)     Off             no filter (the default)
                     A500            the A500 board's fixed filter, always,
                                     and the LED filter while the power LED
                                     is lit
                     A1200           only the LED filter, as on an A1200
   Volume (HDMI)     100 ... 400 %   200 % is the default; above 100 % the
                                     loudest peaks are rounded off, not cut
   Stereo            Amiga           channels 0 and 3 left, 1 and 2 right,
                                     as on a real Amiga (the default)
                     75, 50, 25 %    the two sides mixed into each other
                     Mono            both sides the same
   Left and right    As they are     (the default)
                     Swapped         left on the right, right on the left

Stereo and Left and right change the Amiga's own output too, while the
emulated chipset plays; the rest is HDMI's alone. Every change is heard
at once; Save keeps it for good, Use until you switch off, Cancel puts
it back. Sound on HDMI stays on across reboots once on, and so does HiFi
sound.

The Amiga's own output goes through your Amiga's real audio filters.
HDMI sound comes straight from the emulated chipset and sounds like an
A1200 with its LED filter off - brighter than an A500 - unless Filter
gives it an Amiga's filters. Games and Kickstart switch the LED filter on
and off themselves, by the power LED, as on a real Amiga. The filters
follow WinUAE's model of them.

Paula's four channels each hold a level and change it at exact moments,
far more often than HDMI takes a sample. Classic averages them over each
sample, as every version before 1.114 did. Band-limited builds every
change from a band-limited step instead, so nothing above what you can
hear folds back down as a whistle or a grainy edge - cleaner in high
notes and sharp effects, at any rate. It costs the Pi a little time; it
played fine on a Pi 3 A+.

HIFI SOUND. AHI programs - AmigaAMP, trackers, the Workbench's own sounds
- normally play through Paula, 8 bits a channel. With HiFi sound On they
play straight on HDMI instead: AHI mixes in 32 bits and HDMI plays it in
24 (in 16 on a screen that takes no more), at the program's own rate -
44.1, 48, 88.2, 96, 176.4 or 192 kHz - so music made at 44.1 kHz plays
as it was made. It needs AHI installed (DEVS:ahi.device) and Sound on
HDMI On, and it is heard on HDMI only. On puts agaboot's driver into
DEVS:AHI (aga-hdmi.audio) and its modes into DEVS:AudioModes (AGA-HDMI),
and sets AHI's unit 0 and its Music unit to "AGA-PISTORM HDMI:HiFi 24 bit
stereo++" at 48 kHz - pick 44100 Hz in Prefs/AHI for music from CDs, and
point other units at that mode yourself if you use them. AHI's settings
as they were are kept in ENVARC:AGA-PISTORM/ahi.prefs.before, and Off
gives those units their earlier modes back. A program that is playing
already keeps its old sound until it starts again.

THE RATE. HDMI sound runs at 48 kHz. With HiFi sound, Follow screen lets
it follow an AHI program to any rate your screen lists in its EDID (the
list it gives the Pi), and Follow, any to all six - for an AV receiver or
monitor that plays more than it says; if the sound stops at a high rate,
go back to Follow screen. 96 kHz and 192 kHz hold HDMI at that rate for
everything, games included. When the rate changes, the sound stops for a
moment while the screen locks on to the new one; a game puts HDMI back
at 48 kHz unless the rate is held.

HDMI plays a few milliseconds after the Amiga's own
output, plus whatever delay your TV adds, so with both on at once you may
hear an echo: turn one of them down. Only the emulated chipset reaches
HDMI - a game on the Amiga's own chipset (AGA turned off in Init, or a
program started outside WHDLoad without agaboot RUN or Run program...)
is heard on the Amiga's own output alone. If a game falls behind on a
Pi 3, the sound on both outputs has a short gap at the same moment.

THE SOUND MONITOR. Sound -> Monitor... - also the AGA-SoundMonitor icon
in SYS:Utilities, and agaboot SOUND MONITOR in a Shell - shows what HDMI
plays, as it plays: level meters and a spectrum of the very samples the
screen's speakers get, two VU meters (a click on one moves where 0 VU
sits), a vectorscope with the phase between left and right, a scope, and
Paula's four voices, each with its own scope, note, volume and sample
length. A click on a voice mutes it, the right button hears it alone;
neither is kept. View shows all of it on a screen with room, or one part
at a time. It also shows the state of HDMI and HiFi sound and the
screen's own name, the formats it decodes and its speakers, as its EDID
gives them; it holds the HiFi volume (AHI programs' sound, which the
Settings window's volume leaves alone), and Test plays a sound on the
left, the right or both, through AHI, to check the speakers by. It needs
Sound on HDMI On; Paula's voices show while the emulated chipset plays
(a game, or System wide).

RECORDING. Record, in the Sound monitor, writes what HDMI plays into a
WAV file - 16 bits at HDMI's rate, as the speakers get it - named
AGA-rec-001.wav, -002 and on, in a drawer you pick (RAM: the first time).
While a game runs it writes each of Paula's four voices into a file of
its own as well (AGA-rec-001-v1.wav to -v4.wav, at 27,928 Hz on a PAL
Amiga): a game's music taken apart into its channels, which no recording
of the speakers can do. A disk too slow to keep up loses frames, and the
monitor counts them.

THE EQUALIZER. Sound -> Equalizer... gives sound over HDMI its last
touches, whoever made it - the emulated chipset and HiFi sound alike: a
preamp and ten bands from 31 Hz to 16 kHz (-12 to +12 dB each), headphone
crossfeed (Off, Light, Strong - each side's bass drawn towards the other,
as a room would do it), balance, and a limiter that holds the peaks under
full scale instead of clipping them. Presets: Flat, Bass boost, Treble
boost, Loudness, Vocal, Rock, Pop, Jazz, Classical, Electronic, TV
speakers, Headphones, Warm, Bright, and three of your own (Store). Drag a
fader with the left mouse button; the right button puts it back to 0 dB.
It is heard on HDMI only.

PAULA'S VOICES. Paula wires voices 1 and 4 to the left and 2 and 3 to
the right, hard: fine on speakers set apart, tiring on headphones.
Sound -> Voices... puts each voice anywhere between left and right, at
its own level, on HDMI and on the Amiga's own output alike while the
emulated chipset plays (band-limited Paula on HDMI included). Presets:
Amiga (as Paula wires them), Soft stereo, Headphones, Spread (each voice
a place of its own), Centre (mono) and Swapped. Off, or at Amiga, the
sound is exactly as before.

The Equalizer and Voices windows open beside the Sound monitor, so its
spectrum and scopes show what each fader does. In both, changes are
heard at once; Save keeps them, Use until you switch off, Cancel puts
them back.

From a Shell:

   agaboot SOUND HDMI ON           on (OFF: off)
   agaboot SOUND AHI16 ON          HiFi sound on (OFF: off)
   agaboot SOUND PAULA BANDLIMITED Paula on HDMI: CLASSIC or BANDLIMITED
   agaboot SOUND RATE ANY          SCREEN, ANY, 96 or 192
   agaboot SOUND FILTER A500       the filter: A500, A1200 or OFF
   agaboot SOUND VOLUME 300        HDMI's volume: 100, 150, 200, 300, 400
   agaboot SOUND STEREO 50         AMIGA, 75, 50, 25 or MONO
   agaboot SOUND SWAP ON           left and right swapped (OFF: as they are)
   agaboot SOUND                   what is set, and whether the Pi can play it
   agaboot SOUND MONITOR           the Sound monitor
   agaboot SOUND EQ ON             the equalizer on (OFF: off, ASK: its window)
   agaboot SOUND EQ PRESET Rock    a preset, by name (two words may be typed
                                   as they are: PRESET Bass boost)
   agaboot SOUND EQ BANDS 3,2,0,0,0,0,0,1,2,3
                                   the ten bands, 31 Hz to 16 kHz, -12 to 12;
                                   also PREAMP -12..12, BALANCE -12..12,
                                   LIMITER ON|OFF, CROSSFEED OFF|LIGHT|STRONG
   agaboot SOUND VOICES ON         the voice mixer on (OFF: off, ASK: its window)
   agaboot SOUND VOICES PRESET Soft stereo
                                   a preset, by name
   agaboot SOUND VOICES PAN 1 -4   voice 1 to 4, from -8 (left) to 8 (right)
   agaboot SOUND VOICES LEVEL 1 12 voice 1 to 4, from 0 (silent) to 16 (full)
   agaboot SOUND EQ                what is set (SOUND VOICES the same)

The settings live in ENVARC: - AGA_HDMISOUND, AGA_AHI16, AGA_HDMIPAULA,
AGA_AUDIORATE, AGA_HDMIFILTER, AGA_HDMIVOL, AGA_STEREO and AGA_SWAP; the
equalizer's in AGA_EQ (your presets in AGA_EQUSER1 to 3), the voices' in
AGA_VOICES, the HiFi volume in AGA_HIFIVOL, and the recordings' drawer in
AGA_RECDIR.
Sound on HDMI On also puts hdmi_drive=2 into config.txt's AGA-PISTORM
block, and Off takes it out again: from the next power cycle it makes the Pi
drive the screen as HDMI. Most
screens get HDMI anyway, but one the Pi drives as DVI shows the picture
and carries no sound at all, and this line is what fixes that.

For it the SD card's boot partition - the FAT one with config.txt on it,
whatever it is called on your system - must be mounted, and many systems
do not mount it by themselves. If it is not, the sound is switched all the
same and agaboot tells you: mount it the way your system does (THE BOOT
PARTITION ON AMIGAOS 3.x has a mountlist for a system without one) and
choose Sound again, or add the line on a PC. On a PiStorm16 config.txt is
left alone: add hdmi_drive=2 yourself if your screen needs it.

The Pi needs its HDMI screen when it starts (or hdmi_force_hotplug=1 in
config.txt): without one it leaves HDMI alone, and the Sound window says
so. To keep the Pi off HDMI sound entirely, put aga.nohdmiaudio in
cmdline.txt. Tested on a CM4 and on a Pi 3 A+ (HiFi sound, Band-limited
and the rates on the CM4, HiFi at 44.1 to 96 kHz on the Pi 3 A+); not on
a PiStorm16 yet.


RUNNING ONE AGA PROGRAM
-----------------------
WHDLoad games switch to the emulated chipset by themselves. For an AGA
program that is not a WHDLoad game - a demo started from its own file, a
launcher - choose Chipset menu -> Expert -> Run program... and pick the
program in its file requester, or type in a Shell:

   agaboot RUN Stuff:Demos/EverythingDies/EveryThingDies.executable

agaboot switches the machine to the emulated chipset, starts the program
with whatever arguments follow its name, waits for it to end and switches
back; Run program... does the same with the program you picked. It is
System wide for as long as one program runs, with the same caution: it
can freeze the machine, so save your work first. With System wide
already on, it just runs the program.


GOOD TO KNOW
------------
 * No Framethrower, or an Amiga that cannot take one (an A1000 has no
   sync at its Denise socket)? Everything can still go out on HDMI
   through the emulated chipset: with AGA on (Chipset menu -> Init),
   EVERY WHDLoad game runs on it, OCS and ECS ones too (they get an ECS
   Denise, unless they turn out to draw for AGA);
   other programs with agaboot RUN <program> or Chipset menu ->
   Expert -> Run program...; the whole Workbench with Chipset menu ->
   Expert -> System wide. Pick "Keep it on (A1kmode)" there and it
   comes back at every boot; hold the left mouse button while the Amiga
   starts to skip it once. "AGA from boot" in the same place goes one
   step further: the Amiga reboots once onto the emulated chipset, and
   AmigaOS itself then has AGA's screen modes until you switch off.
 * Floppy drives - DF0: or a Gotek - work while System wide is on:
   reading is tested, writing not yet.
 * Only WHDLoad games switch to the AGA chipset. A game that starts
   straight from Workbench (Reshoot Proxima 3, for example) runs on
   the Amiga's own chipset and will look garbled.
 * BUT if you convert it to WHDLoad it will work * - or try starting it
   with agaboot RUN or Chipset menu -> Expert -> Run program... (see
   RUNNING ONE AGA PROGRAM).
 * FullMoon (Virtual Dreams, WHDLoad): its 3D dot graph stands still.
   It runs code that it rewrites every frame, with the instruction cache
   on, and Emu68 goes on running the code as it was before. Give
   the game's icon the WHDLoad tooltype NoCache and the dots move again
   (found by RGB2RTG and checked on an A1200).
 * If your Framethrower is set to "boot" (dtoverlay=unicam,boot,...),
   its live picture replaces the Emu68 logo at power-on, so you will
   not see the "AGA-Pistorm for ECS" line there. Nothing is wrong with that.
 * Scanlines, or a picture off to one side, that RTG Image's Default
   does not take away? With a Framethrower, RTG Image only sets the
   picture of what runs on the emulated chipset (WHDLoad games, with
   AGA on). Everything on the Amiga's own chipset reaches HDMI as the
   Framethrower's picture, with its own scanlines and position: the
   Workbench's native screens, games started from Workbench or an ADF,
   and every game while AGA is off. Those are set on the
   dtoverlay=unicam line in config.txt: sc= and scl= are its scanlines
   (0 the strongest, 4 none), w= with x= (and h= with y=) crop the
   picture and move it. Copy config.txt before you change it.
 * Unintended side effect which i will not disclose is when you run ECS 
   games in this mode => watch for RTG
 * Moved the card between a Pi 3 and a Pi 4? Run the installer again -
   each Pi has its own kernel. Chipset menu -> Init reminds you.
 * HDMI goes black when a game starts? Chipset menu -> Output -> RGB
   port is set to "RGB port only": the game is on the Amiga's own RGB
   port. Pick "HAM6 and HDMI" or Off to have it on HDMI again.
 * The RGB port picture is HAM6: sharp edges between very different
   colours can show a pixel or two of colour fringe. That is how HAM
   works on the Amiga's own chipset; HDMI has the full picture. Palette
   32 colours has no fringes, but only 32 colours for the whole frame.
 * The flicker fixer puts the two halves of an interlaced picture
   together, line by line, so something moving fast on an interlaced
   screen shows comb-like edges. Flicker fixer: Off shows each half as
   it comes instead.
 * Video: NTSC on a 50 Hz HDMI mode: 60 pictures a second do not fit
   evenly into 50, so scrolling judders a little.
 * The TV border does not shrink the Framethrower's live picture.
 * An experiment: add aga.sleep to the end of the options line in
   EMU68:boot/cmdline.txt and the Pi's cores sleep through their timed
   waits instead of spinning. It saves little while a game runs - half a
   degree on a Pi 3 A+, since the cores that run the game keep running -
   and more while the RGB port waits or AHI plays on the desktop. Off
   by default; take it out again if anything misbehaves.
 * PiStorm3D (MiniGL on the Pi's own 3D chip) needs a Pi 4 or a CM4, and
   it is not tested with AGA-PISTORM's kernel. If it only shows a blank
   screen, the Picasso96 libraries are probably of mixed versions -
   RGB2RTG found CaffeineOS's that way - and those of Picasso96 3.6.3
   put it right.


UNINSTALLING
------------
Double-click Uninstall in the AGA-Pistorm drawer, then power cycle. It
puts back the previous kernel line in config.txt, the previous RTG
driver, the WHDLoad scripts and the menu. The kernel file and the
overlays stay on the boot partition; nothing uses them any more.


IF SOMETHING GOES WRONG
-----------------------
 * Chipset > Init says the kernel running is older than this
   AGA-PISTORM? The Amiga was reset with Ctrl-Amiga-Amiga after
   installing, so the old kernel is still running. Switch it off, wait a
   few seconds and switch it on again.
 * AmiKit: AGA games say "This game requires AGA chipset" even though
   Init is on? WHDLoad was most likely updated since you installed, and
   the new C:WHDLoad replaced AGA-PISTORM's stand-in. Run Install again
   and pick AmiKit (see AMIKIT AND MULTIBOOTOS).
 * A game's sound crackles while it loads (the intro, a new level),
   mostly on a Pi 3? WHDLoad is reading the game's files from the card
   while you play. Turn on WHDLoad's Preload: the tool type PRELOAD on
   the game's icon, or a line Preload in S:WHDLoad.prefs (see AMIKIT AND
   MULTIBOOTOS).
 * The Amiga no longer starts after installing: switch it off and put
   the card in a PC (if Windows offers to format a drive, click Cancel -
   see STEP 1). Copy config.txt.pre-aga over config.txt on the boot
   partition: that starts your previous kernel again. Put the card back
   in the Amiga and switch on. Only if the desktop does not come up
   (your old kernel with the new VideoCore 1.5): hold both mouse buttons
   while it starts, choose Boot With No Startup-Sequence, and in the
   Shell type
       CD SYS:Libs/Picasso96
       Copy VideoCore.card.pre-aga VideoCore.card
   then reboot. Either way, finish with a double-click on Uninstall in
   the AGA-Pistorm drawer: it puts back the RTG driver, the WHDLoad
   scripts and the menu (see UNINSTALLING), and finds nothing left to do
   in config.txt. If it cannot find the boot partition, Skip is fine -
   config.txt is back already - but then it leaves the RTG driver as it
   is too, so do the Copy above in a Shell if you have not.
 * The installer says the Emu68 boot partition is READ-ONLY, and
   SD-Setup.cmd did not fix it (it leaves config.txt alone when it cannot
   tell which Emu68 the card has): Emu68 1.1 lets the Amiga write there
   only when config.txt says so. On a PC, open config.txt on the boot
   partition and put these lines near the top, before its first line that
   starts with [ - each on a line of its own:
       [pi4]
       dtoverlay=emmc,unit0=rw
       [pi3]
       dtoverlay=sdhc,unit0=rw
       [pi02]
       dtoverlay=sdhc,unit0=rw
       [all]
   That closing [all] matters: without it, the rest of the file only
   counts on a Zero 2 W. Putting them at the end of the file instead?
   Start them with an [all] line too: a [pi4] does not close a section
   like [gpio17=0] above it, the Pi takes both together, and the line
   under it would only count when both match. A PiStorm16 card needs
   only the first two lines and the [all] (a PiStorm16 carries a Pi 4
   or a CM4, see PiStorm16.txt).
   With an older Emu68 1.0.x the switches are different: add
       sd.unit0=rw emmc.unit0=rw
   to the end of the command line file - the one config.txt's cmdline=
   line names, boot/cmdline.txt on CaffeineOS. SD-Setup.cmd does that one
   by itself.
 * WHDLoad games no longer switch to AGA, because the AGA-PISTORM lines
   in S:WHDLoad-Startup or S:WHDLoad-Cleanup are gone: in a Shell, type
   C:agaboot INSTALL HOOKS. It puts them back (and the Chipset menu line
   in S:User-Startup) without the rest of the installer, keeping a
   .pre-aga copy of each file. "CANNOT READ" means that script itself is
   missing - it comes with CaffeineOS's WHDLoad setup. On AmigaOS 3.x,
   C:agaboot INSTALL HOOKS CREATE makes the missing scripts first.
 * A game shows a grey screen or hangs: wait a bit first. If it stays
   stuck, quit with F10 if you can, otherwise press Ctrl-Amiga-Amiga.
   Power cycle and try again.
 * A game ignores the joystick: some games do not accept a CD32 pad -
   they do the same on a real A1200. Try a normal one - or two-button
   joystick.
 * The RGB port stays dark during a game: check Chipset menu -> Output ->
   RGB port is not Off. It needs 162 KB of chip RAM free when the game
   starts, and the Pi's second core - it cannot work with async_log in
   EMU68:boot/cmdline.txt. The picture cut off at the bottom of an LCD:
   move it with Position..., or the scaler's own vertical position.
 * Games stutter, the sound skips, or a demo hangs - but only after
   playing for a while: the Pi is probably too hot and slowing itself
   down (a CM4 does that at 80 C; a Pi 3 B+ or 3 A+ already at 60 C, or
   70 C with the temp_soft_limit=70 the installer puts under [pi3] when
   you let it).
   Give it a heatsink and a fan. Also
   look at the end of config.txt on the boot partition: CaffeineOS has an
   "Overclock Pi3" block there whose lines are NOT commented out, and
   without a [pi3] line above them they apply to every Pi - +100 mV and a
   faster bus clock (core_freq=550) on a Pi 4 or CM4 too. Put a # in
   front of those five lines, and make sure force_turbo=1 is in the
   [pi4] section instead.
 * Files or a partition damaged after writing on a Pi 4 or CM4 - a file
   that reads back wrong, PFS3 reporting a bad block: some SD cards drop
   a write now and then at the full 50 MHz SD speed. It happens with
   Emu68 1.0.x and 1.1 alike (found by RGB2RTG on two cards). Slowing
   the card to 25 MHz cures it. Easiest: run Install again and answer
   Yes to "Run the SD card at 25 MHz?" - it puts emmc.low_speed into
   the command line file, which the AGA-PISTORM kernel reads since
   1.116, and Uninstall takes it out again. By hand: in config.txt on
   the boot partition, add low_speed to the overlay lines, so they read
       dtoverlay=sdhc,low_speed,unit0=rw
       [pi4]
       dtoverlay=emmc,low_speed,unit0=rw
   (with Emu68 1.0.x, add sd.low_speed emmc.low_speed to the command
   line file instead). Copy your work off the card first and check it.
 * The desktop comes up on the Amiga's own screen and ScreenMode lists
   no VideoCore modes, after moving to Emu68 1.1 or installing: the
   VideoCore driver 1.5 that Emu68 1.1 needs tells Picasso96 it is a new
   kind of board, so settings saved with an older VideoCore (1.2.x) no
   longer match it. Since 1.116 the installer sees this coming and adds
   the tooltype VC4_LEGACY_ID to the VideoCore monitor's icon itself
   (agaboot CHECK says whether it will). If you moved to Emu68 1.1
   without it, add VC4_LEGACY_ID to that icon in DEVS:Monitors yourself
   and reboot: the driver answers as the old one did, and your settings
   come back (found by RGB2RTG, checked on AmigaOS 3.2).


CREDITS AND LICENCES
--------------------
AGA-PISTORM by Astair86, made for the Amiga community.

 * AGA-PISTORM's code - the AGA chipset emulation and the Amiga tools
   (agaboot, agastat) - is free software under the GNU General Public
   License, version 2 only (Licenses\GPL-2.0.txt).
 * The chipset emulation contains code from UAE and WinUAE (both GPL; Bernd
   Schmidt, Alessandro Bissacco, Toni Wilen and the other UAE authors) -
   some copied, more translated into C - and elsewhere follows their way of
   doing things:
     - blitter: the fill step (copied); normal blits and the line mode
       (translated); the fill table; the cycles per word
     - copper: the classic UAE copper (translated); the WAIT/SKIP compare
     - playfield: the fetch tables, fetch modes, display window, delay
       offset, BSCAN2 modulos and dual playfield split (copied); colour
       writes, register handlers and HAM (translated); UAE's fetch and line
       model
     - sprites: start/stop and the DMA cycle steps (copied); SPRES and
       VSTART/VSTOP (translated)
     - collisions: the CLXDAT read (copied); the CLXCON masks (translated)
     - custom registers: setclr and a few short lines
     - disk: the DD track length and gap; the DSKBYTR bits
     - audio: four interrupt and timing rules, rewritten; no code copied
   Much of it is the older UAE/WinUAE code as kept in E-UAE, Richard
   Drummond's UAE port, from before WinUAE's 2024-2025 rewrite of
   custom.cpp and drawing.cpp. The timing was checked against WinUAE's DMA
   records, and WinUAE was the reference every frame was compared with.
   WinUAE is by Toni Wilen (https://www.winuae.net, source
   https://github.com/tonioni/WinUAE), built on UAE by Bernd Schmidt and the
   many UAE authors. Each chipset source file (core\src in Source\) says
   what it took, with the copyright lines of the files it came from;
   Licenses\UAE-WinUAE-notice.txt collects them. Thank you!
 * PiStorm by Claude Schwarz aka captain-amygdala
   (https://github.com/captain-amygdala/pistorm) - the hardware all of this
   runs on.
 * PiStorm600 by LemaruX (https://github.com/LemaruX/PiStorm600) - the
   PiStorm for the A600.
 * The kernel is Emu68 by Michal Schulz, with contributions from
   CrypticalCode0, MBeijer, mras0, rondoval, captain-amygdala, ronaegis,
   apolkosnik, mdpye and TomCat666
   (also a side note: Claude Schwarz aka captain-amygdala and Michal Schulz -
   the inventions of these two are the reason i came back to Amiga)
   - https://github.com/michalsc/Emu68 - under the Mozilla Public License 2.0
   (Licenses\MPL-2.0.txt), with the AGA-PISTORM chipset built in.
   This is an UNOFFICIAL build: please do not report its problems to the
   Emu68 project. Its source is Emu68 1.1.0-beta.1 (git 0ba3a89) plus the
   AGA-PISTORM patch in Source\ (arm\emu68-aga.patch); the complete Emu68
   source it is built from is published next to this package, as
   AGA-PISTORM-1.106-emu68-src.tar.gz, on the package's download page:
   https://astair86.itch.io/aga-pistorm-10-experimental-prototype
   Emu68's own files stay MPL 2.0, and
   the changes the patch makes to them are MPL 2.0 too. The kernel as a
   whole - Emu68 with AGA-PISTORM's GPL code built in - is distributed under
   the GNU General Public License, version 2 (Licenses\GPL-2.0.txt), as the
   MPL 2.0 allows (its section 3.3).
 * Emu68 builds these libraries into the kernel:
     Capstone disassembly framework by Nguyen Anh Quynh, (c) COSEINC,
       BSD licence (Licenses\Capstone-LICENSE.txt, Capstone-LICENSE_LLVM.txt)
     libdeflate by Eric Biggers, MIT licence (Licenses\libdeflate-COPYING.txt)
   and more code whose notices are in Licenses\Emu68-third-party-notices.txt:
   the PiStorm bus protocol (MIT), libxsvf (ISC), musl's and Sun's maths,
   FreeBSD's exp2, NetBSD's qsort, the PowerPC ROM (MIT), and AROS's TLSF
   allocator, topaz font and exec lists. It also carries, as data built
   elsewhere, the Emu68 ROM with its Amiga modules and the PiStorm16's FPGA
   firmware (that file says what is known about them).
 * The overlays are Emu68's by Michal Schulz, MPL 2.0, unmodified: all but
   logo.dtbo are built from Emu68 0ba3a89's src\overlays (in the Emu68
   source next to this package); logo.dtbo is Emu68 git 3cd482d's logo
   overlay, its source in Source\ (package\overlays-src\logo.dts).
 * VideoCore.card is from Emu68-tools by Michal Schulz, MPL 2.0, with my
   changes - their source is in Source\ too.
 * The CPLD firmware (the separate CPLD download) comes unchanged from
   LemaruX's PiStorm-Firmware (https://github.com/LemaruX/PiStorm-Firmware).
 * The WHDLoad slaves for games that are not WHDLoad games (Reshoot and the
   demos) are built from kick31.asm by Wepl - WHDLoad, https://whdload.de.
 * The RGB port's choice of sizes and steady 25 frames a second, and
   agaboot RUN, follow ideas from Future Retro Fusion's PiStorm-AGA-HAM6
   (https://github.com/futureretrofusion/PiStorm-AGA-HAM6) - thank you!
 * gtv3000 reported the Castlevania AGA background that led to the
   SSCAN2 sprites in 1.111 - thank you!
 * Sound over HDMI (arm\aga_hdmi.c) follows the Linux kernel's VC4 HDMI
   audio driver, drivers/gpu/drm/vc4/vc4_hdmi.c from the Raspberry Pi
   kernel (GPL-2.0-only; Copyright (C) 2015 Broadcom, (c) 2014 The Linux
   Foundation, (C) 2013 Red Hat): its start, stop and register values,
   rewritten for Emu68 with a DMA ring of its own.
 * HiFi sound's driver (aga-hdmi.audio, which agaboot carries; its source
   is tools\ahi\aga-hdmi.c in Source\) is written for AHI by Martin Blom,
   on the model of the Void and Device drivers in AHI's developer kit.
 * The equalizer's filters (arm\aga_fx.h) are the peaking filters of
   Robert Bristow-Johnson's Audio EQ Cookbook, worked out in fixed point.
 * AngelClaww tested the installer on AmigaOS 3.2 with a PiStorm16 and
   found the missing executable bit, the CONFIG.TXT case, the PiStorm16
   clocks; the PiStorm16 kernel-line-only install and the safe remove
   are AngelClaww's ideas - thank you!
Awesome OS that Capt. LeChuck (ko-fi.com/captlechuck) made for us, thank you!


Lastly thanks to the Emu68, PiStorm and CaffeineOS projects/community! 
Emu68 already made the PiStorm a affordable accelerator. 
Now You can play A1200 games, at a time when the real hardware costs a fortune.

NOTICE! USE IT AT YOUR OWN RISK
---------------------------------
It's not a system wide AGA ,this is going to be run in an sandboxed mode 
your system will still report as OCS/ECS system, ADF's will not work! 
this will not change. Also not everything is tested, from what i can tell 
most of the WHDLoad game work. Non WHDLoad games will NOT work!
If you notice that kind of case make a WHDload out of it and it will probably work.
(for example i've tested reshoot proxima 3 game that i own and that isn't whdload)
This was only tested on an heavily modified dusty A500 rev6 upgraded to 2MB of chip with ACE2B 
(from my attic after my beloved A500+ died on me). 
1.1 was also tested on an A500+ with 1MB of chip, with a CM4 and with a Pi 3.
1.107 (the RGB port, scale and scanlines) was tested with a Pi 3 A+.
1.112 (sound over HDMI) was tested with a CM4 and with a Pi 3 A+.
1.113 was tested with a CM4.
1.114 was tested with a CM4 and with a Pi 3 A+.
Users report it works on an A1000 with a Rejuvenator board too.
Should work on any ECS machine with 1 or 2MB of chip => Im looking at you A600! 
But i havent tested since i dont have those other machines and i'm a one band guy. 
Hopefully all of this won't anger some ppl but its the internet and there are "interests" out there. 
Constructive remarks i will engage with, negative ones i will ignore life is too short for that to occupy my time. 
I will not be on forums or boards or whatever....
BEFORE DOING ANYTHING MAKE A BACKUP OF YOUR EMU68 SDCARD! 


AI
--

AI was heavily used for debugging this and since i have a life and haven't got infinite time 
it is what it is, if you dont like it dont use it! 


ROADMAP
-------
This will get updates with the new emu68 kernel. IF and when I find time for it (and when i repair my other A500).
This is by no means a polished thing, I am not a professional in this field this is a hobby. 
The source code is in Source\, with the Emu68 source the kernel is built from (AGA-PISTORM-1.106-emu68-src.tar.gz)
and VideoCore.card's; Source\HOW-TO-BUILD.txt tells how to build it all.
New PPC kernel that is being worked on by the creator of the Emu68 is mostly probably going to be 
compatible with this (i've noticed some levers in the code and made a switch that will be implemented 
when PPC comes online until then its there to be used when the time comes) 
(but its going to be either this or that since it both uses the same "core 3" of the pi4.)
*Not the case of having a cake and eating it too*
512 KB chip machines (A1000, A2000, A500 rev 3/5/6 without a trapdoor card) - 1 MB works since 1.1
There are also bugs which will be discovered i'm sure. There are some of that i know of 
(graphical corruption in the text overly=>Reshoot proxima 3)
Those might get fixed. 

On another note one of my beloved A500+ died while making this and i need advice or money for repair 
(which i have no idea where and how?) so any kind of donation would do, coffee/beer also works, 
otherwise its FREE FOR ALL as it should be, hopefully it will bring some smiles into this world, Cheers!



Special thank you to katzazakis, FraBro and Angelclaww for testing!

Discord server with awesome ppl can be found here: https://discord.gg/b5kuSrDZgs

 






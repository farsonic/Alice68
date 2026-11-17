ALICE68: Alice68's Amiga files
==============================

This drawer is the folder ALICE68 on the SD card's FAT partition (next to
kernel8.img). alice68hdf.device mounts it as the volume ALICE68: at every
boot, so new versions of these files arrive by copying the folder onto the
card from a PC.

  C/Alice68              the Shell command (Alice68 HELP lists what it does)
  Prefs/Alice68Prefs     the settings window (MUI 3.8 or later)
  WBStartup/Alice68Wheel starts the mouse wheel helper with Workbench
  DEVS/USBHardware/alice68-usb.device   for Neptune or Poseidon
  DEVS/alice68net.device the network card (WiFi unit 0, Ethernet unit 1)

Alice68Prefs has pages for the kernel's status, cmdline.txt's settings,
config.txt, the SD card and its files, the network (WiFi and Ethernet),
Bluetooth, the joystick, the logs and a monitor. Save writes cmdline.txt
and config.txt (keeping .bak copies); Restart Pi restarts with them.

The Shell command does the same without a window:

  Alice68 STATUS [LOOP]       the kernel: cores, temperature, JIT, network
  Alice68 CARD ...            partitions, files, OPTIONS, SET, BOOT, KICKSTART,
                              CONFIG, CMDLINE, RESTART
  Alice68 BT ...              Bluetooth: LIST, SCAN, PAIR, CONNECT, FORGET, KEEP
  Alice68 WIFI ...            WiFi: SCAN, JOIN "name" "password", RECONNECT ...
  Alice68 LOG [LINES=n] [MATCH=text] [FOLLOW]   the kernel's and USB driver's logs
  Alice68 JOYTEST [ONCE]      the joystick ports as games read them, live
  Alice68 HDF                 the hardfiles and folders found, and why
  Alice68 NETCHECK            what the network driver reports
  Alice68 SHOWCONFIG          boards, Alice68's cards, memory
  Alice68 MONITOR [DUMP]      a debugging window (DUMP: as text)
  Alice68 WHEEL               a mouse's scroll wheel (run it again: stop it)

The wheel helper turns a USB or Bluetooth mouse's wheel into the NewMouse
events most programs know (wheel up/down/left/right, the 4th button).
Install puts Alice68Wheel in WBStartup, so it starts with Workbench.

Install also takes away the separate programs of Alice68 0.6 and before
(C/Alice68-Status, Alice68-Card, Alice68-Network ...): they are all in
C:Alice68 and Alice68Prefs now. Uninstall puts back what they replaced.

Icons/ holds volume icons: copy one onto a drive as its Disk.info. Folders
and CD images show theirs by themselves; for a hardfile's partition:

  Copy ALICE68:Icons/Hardfile.icon DH0:Disk.info

Releases also carry Riptide, a TCP/IP stack that uses alice68net.device,
in the Riptide drawer here: double-click Install_Riptide there.

To put them into the system, double-click Install (it runs the Installer),
or from a Shell:

  Installer ALICE68:Install

It copies each file to SYS: (Expert users can choose another place) and
keeps what it replaces in Alice68-Backup there, the first time only, so that
Uninstall puts the system back as it was before Alice68's first install.
Run Install again after copying a newer ALICE68 folder onto the card, and
reboot afterwards so that the new devices are loaded.

Without the Installer (it is on AmigaOS 3.1's Install disk, and in 3.2 and
3.1.4): Execute ALICE68:Install-Shell, and Execute ALICE68:Uninstall-Shell.
Workbench shows the C and DEVS drawers with Window > Show > All Files.

Other folders of the card can be mounted too, in config.txt:

  alice68.folder=Games              the folder Games as volume Games:
  alice68.folder=Amiga/Work:Work    Amiga/Work as volume Work:
  alice68.folders=off               none, not even ALICE68:

They are read-write, never bootable. FAT keeps no comments and no Amiga
protection bits other than write/delete protection; hardfiles (*.hdf) that
are mounted cannot be changed through a folder.

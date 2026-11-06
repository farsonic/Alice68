ALICE68: Alice68's Amiga files
==============================

This drawer is the folder ALICE68 on the SD card's FAT partition (next to
kernel8.img). alice68hdf.device mounts it as the volume ALICE68: at every
boot, so new versions of these files arrive by copying the folder onto the
card from a PC.

  C/Alice68-HDF          the hardfiles and folders found, and why
  C/Alice68-USBLog       the USB pass-through's log
  C/Alice68-NetCheck     what the network driver reports
  C/Alice68-WiFi         WiFi from the Shell
  C/Alice68-Network      the network settings window (MUI)
  C/Alice68-NetworkGT    the same without MUI (GadTools)
  C/Alice68-Monitor      a debugging window
  DEVS/USBHardware/alice68-usb.device   for Neptune or Poseidon
  DEVS/alice68net.device the network card (WiFi unit 0, Ethernet unit 1)

To put them into the system, open a Shell and type

  Execute ALICE68:Install

It copies each file to SYS: (or TO=<somewhere>:) and keeps what it replaces
in SYS:Alice68-Backup, the first time only, so that

  Execute ALICE68:Uninstall

puts the system back as it was before Alice68's first install. Run Install
again after copying a newer ALICE68 folder onto the card; reboot afterwards
so that the new devices are loaded.

Other folders of the card can be mounted too, in config.txt:

  alice68.folder=Games              the folder Games as volume Games:
  alice68.folder=Amiga/Work:Work    Amiga/Work as volume Work:
  alice68.folders=off               none, not even ALICE68:

They are read-write, never bootable. FAT keeps no comments and no Amiga
protection bits other than write/delete protection; hardfiles (*.hdf) that
are mounted cannot be changed through a folder.

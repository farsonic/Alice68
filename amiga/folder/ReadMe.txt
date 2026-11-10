ALICE68: Alice68's Amiga files
==============================

This drawer is the folder ALICE68 on the SD card's FAT partition (next to
kernel8.img). alice68hdf.device mounts it as the volume ALICE68: at every
boot, so new versions of these files arrive by copying the folder onto the
card from a PC.

  C/Alice68-HDF          the hardfiles and folders found, and why
  C/Alice68-USBLog       the USB pass-through's log
  C/Alice68-NetCheck     what the network driver reports
  C/Alice68-ShowConfig   ShowConfig and more: boards, Alice68's cards, memory
  C/Alice68-WiFi         WiFi from the Shell
  C/Alice68-Network      the network settings window (MUI)
  C/Alice68-NetworkGT    the same without MUI (GadTools)
  C/Alice68-Monitor      a debugging window
  C/Alice68-Wheel        a USB mouse's scroll wheel (also in WBStartup)
  DEVS/USBHardware/alice68-usb.device   for Neptune or Poseidon
  DEVS/alice68net.device the network card (WiFi unit 0, Ethernet unit 1)

Alice68-Wheel turns a USB mouse's wheel into the NewMouse events most
programs know (wheel up/down/left/right, the 4th button). Install puts it in
WBStartup too, so it starts with Workbench; run it again to stop it. Without
installing: Run >NIL: ALICE68:C/Alice68-Wheel

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

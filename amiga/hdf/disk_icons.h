/* SPDX-License-Identifier: GPL-2.0-only
 * alice68hdf.device: the Disk.info shown on the volumes it mounts that have none of their
 * own (folders.c, isos.c). The data: amiga/folder/mkdiskicons.py, at build time. */
#ifndef ALICE68_DISK_ICONS_H
#define ALICE68_DISK_ICONS_H
extern const unsigned char disk_icon_folder[], disk_icon_cd[];
extern const unsigned long disk_icon_folder_len, disk_icon_cd_len;
#define DISK_ICON_NAME "Disk.info"
#endif

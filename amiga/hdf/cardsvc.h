/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice68hdf.device's card service: the SD card Alice68 booted from, for
 * Alice68-Card (amiga/card.c). Open the device with unit CS_UNIT, then DoIO()
 * one of the commands below from a process. Writes go through the device's
 * own FAT and exFAT engines, one at a time with its folder volumes, so the
 * card stays consistent whatever else is open. Mounted hardfiles and CD
 * images, files a folder volume has open and folder volumes' roots are never
 * changed (CSE_INUSE).
 *
 * Volumes: CS_FAT the FAT partition Alice68 booted from (cmdline.txt,
 * config.txt, kernels, ROMs, HDF/, ISO/, WIFI/, ALICE68/); CS_EXFAT the card's
 * first exFAT partition, written only when alice68.exfat=rw (or boot).
 * Paths: from the partition's root, '/' between names, "" the root itself.
 * Every structure: 32-bit fields and char arrays in multiples of 4 only.
 */
#ifndef ALICE68_CARDSVC_H
#define ALICE68_CARDSVC_H

#define CS_UNIT      0x41363843u            /* 'A68C' */
#define CS_VERSION   1u

#define CS_INFO      9                       /* io_Data: struct cs_info; io_Offset 1: count free space too (slower) */
#define CS_LIST      10                      /* io_Data: struct cs_list, io_Length its size */
#define CS_READ      11                      /* io_Data: struct cs_file (path, max), io_Length its size */
#define CS_WRITE     12                      /* io_Data: struct cs_file (path, len, data): made or replaced */
#define CS_DELETE    13                      /* io_Data: struct cs_file (path): a file, or an empty folder */
#define CS_RENAME    14                      /* io_Data: struct cs_file (path -> path2), also into another folder */
#define CS_MKDIR     15                      /* io_Data: struct cs_file (path) */

#define CS_FAT       0u
#define CS_EXFAT     1u
#define CS_PATH      256

/* io_Error beyond the standard IOERR_* */
#define CSE_NOTFOUND 1
#define CSE_EXISTS   2
#define CSE_FULL     3
#define CSE_INUSE    4                       /* mounted, open, or a folder volume */
#define CSE_RO       5                       /* the card, or the exFAT partition, is read-only */
#define CSE_IO       6
#define CSE_BADNAME  7
#define CSE_NOTEMPTY 8
#define CSE_NOVOL    9                       /* no such partition */
#define CSE_TOOBIG   10                      /* the file is larger than the buffer (CS_READ: len says how large) */
#define CSE_ISDIR    11
#define CSE_NOTDIR   12

/* partition kinds */
#define CSP_EMPTY    0u
#define CSP_FAT16    1u
#define CSP_FAT32    2u
#define CSP_EXFAT    3u
#define CSP_AMIGA    4u                      /* type 0x76: an RDB disk inside (Emu68's own) */
#define CSP_EXTENDED 5u
#define CSP_OTHER    6u

struct cs_part {
    unsigned int type;                      /* the MBR's partition type byte */
    unsigned int kind;                      /* CSP_* */
    unsigned int boot;                      /* the MBR's active flag */
    unsigned int start, count;              /* card sectors */
    unsigned int vol;                       /* CS_FAT, CS_EXFAT, or ~0: not one of Alice68's */
    unsigned int total_kb, free_kb;         /* FAT/exFAT: size and free space (free_kb ~0: not counted) */
    char         label[16];
};                                          /* 48 bytes */

#define CS_MAXPART   8
#define CS_MAXUSED   24
struct cs_info {
    unsigned int version;                   /* CS_VERSION */
    unsigned int card_ro;                   /* the card's unit 0 is read-only (dtoverlay=emmc,unit0=ro) */
    unsigned int cmd64;                     /* the card can be read past 4 GB */
    unsigned int nparts;
    struct cs_part part[CS_MAXPART];
    unsigned int exfat_rw;                  /* the exFAT partition may be written */
    char         exfat_why[80];             /* else why not */
    char         card_dev[32];              /* brcm-emmc.device ... */
    unsigned int nused;                     /* files mounted from the card (hardfiles, CD images) */
    struct { unsigned int vol, kind; char name[64]; } used[CS_MAXUSED];   /* kind 1 hardfile, 2 CD image */
};

#define CSF_DIR      1u
#define CSF_INUSE    2u                     /* a mounted hardfile or CD image, or open on a folder volume */
#define CSF_RO       4u                     /* FAT's read-only attribute */
struct cs_ent {
    char         name[108];
    unsigned int size, sizehi;              /* bytes (sizehi: exFAT files past 4 GB) */
    unsigned int date;                      /* FAT date << 16 | time */
    unsigned int flags;                     /* CSF_* */
};                                          /* 124 bytes */

struct cs_list {
    unsigned int vol;
    char         path[CS_PATH];             /* the folder */
    unsigned int max;                       /* entries ent[] has room for */
    unsigned int count;                     /* entries returned */
    unsigned int more;                      /* 1: the folder had more than max */
    struct cs_ent ent[1];                   /* ... max of them */
};

struct cs_file {
    unsigned int vol;
    char         path[CS_PATH];
    char         path2[CS_PATH];            /* CS_RENAME: the new path */
    unsigned int len;                       /* CS_WRITE: bytes in data; CS_READ: the file's size */
    unsigned int max;                       /* CS_READ: room in data */
    unsigned char data[4];                  /* ... */
};
#endif

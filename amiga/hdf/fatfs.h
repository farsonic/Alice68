/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 folders: reading and writing files on the SD card's FAT16/FAT32
 * boot partition, for the folders alice68hdf.device mounts as Amiga volumes
 * (config.txt alice68.folder=...). Long names (VFAT) are read and written;
 * names are Latin-1 on the Amiga side, UCS-2 on the card.
 *
 * Host-independent like hdf_core.c (it uses its struct fat and hdf_io), so
 * tools/hdf-test runs it on a PC against cards made by mkfs.fat and checks
 * the result with fsck.fat and mtools. One caller at a time: the Amiga side
 * holds a semaphore around every call.
 *
 * Writes go through small write-back caches (one directory sector, a window
 * of the FAT); ffs_sync() puts them on the card, every FAT copy included.
 */
#ifndef ALICE68_FATFS_H
#define ALICE68_FATFS_H
#include "hdf_core.h"

#define FFS_NAME     108                /* Amiga names: up to 107 characters */
#define FFS_FATWIN   16u                /* FAT sectors cached */

enum {
    FFS_OK = 0,
    FFS_END = -1,                       /* no more entries / not found */
    FFS_EXISTS = -2,
    FFS_FULL = -3,                      /* no free cluster */
    FFS_DIRFULL = -4,                   /* the FAT16 root directory has no free entry */
    FFS_NOTEMPTY = -5,
    FFS_BADNAME = -6,
    FFS_IO = -7,
    FFS_WP = -8,                        /* the card is write-protected */
    FFS_ISDIR = -9,
    FFS_NOTDIR = -10,
    FFS_LOOP = -11,                     /* a directory moved into itself */
    FFS_CORRUPT = -12,
};

#define FFS_A_RO     0x01
#define FFS_A_HIDDEN 0x02
#define FFS_A_SYSTEM 0x04
#define FFS_A_DIR    0x10
#define FFS_A_ARCH   0x20

/* a directory entry as found. Directories are named by their first
   cluster; 0 is the partition's root (FAT16's fixed area or FAT32's) */
struct ffs_ent {
    uint32_t dir;                       /* the directory it is in */
    uint32_t idx;                       /* index of its 8.3 entry there */
    uint32_t nlfn;                      /* long-name entries just before it */
    uint32_t sec, slot;                 /* card sector and slot (0..15) of the 8.3 entry */
    uint32_t first, size;               /* first cluster (0 = none), bytes */
    uint8_t  attr;
    uint16_t date, time;                /* modified */
    char     name[FFS_NAME];            /* the long name, else the 8.3 one */
    char     sfn[13];                   /* the 8.3 name, as "NAME.EXT" */
};

struct ffs_file {
    struct ffs_ent e;
    uint32_t ci, cc;                    /* the file's ci-th cluster is cc (ci ~0: none known) */
    int      dirty;                     /* the entry needs writing */
};

struct ffs {
    struct fat *f;
    uint8_t *dsec;                      /* directory sector */
    uint32_t dsec_no;                   /* its card sector, ~0 = none */
    int      dsec_dirty;
    uint8_t *fwin;                      /* FAT window, FFS_FATWIN sectors of the first FAT */
    uint32_t fwin_first, fwin_dirty;    /* ~0 = none; one bit per sector */
    uint8_t *bounce;                    /* one sector for partial transfers */
    uint32_t dc_dir, dc_ci, dc_clus;    /* directory walk: dc_dir's dc_ci-th cluster is dc_clus */
    uint32_t hint;                      /* where to look for a free cluster */
    uint32_t nfree;                     /* free clusters, ~0 = not counted yet */
    int      fsinfo_done;               /* FAT32 FSInfo marked "count unknown" */
    uint32_t fsinfo_free;               /* the count last written there, ~0 none */
    int      err;                       /* the first card error since ffs_sync */
    uint16_t date, time;                /* "now", for new and changed entries: the caller sets it */
};

int  ffs_init(struct ffs *v, struct fat *f);
void ffs_done(struct ffs *v);
/* write what is cached; 0 ok, else the first error since the last sync */
int  ffs_sync(struct ffs *v);

/* directories */
int  ffs_next(struct ffs *v, uint32_t dir, uint32_t *pos, struct ffs_ent *e);    /* from *pos (0 = start) */
int  ffs_find(struct ffs *v, uint32_t dir, const char *name, uint32_t len, struct ffs_ent *e);
int  ffs_parent(struct ffs *v, uint32_t dir, uint32_t *parent);                   /* from its ".." entry */
int  ffs_dir_ent(struct ffs *v, uint32_t dir, struct ffs_ent *e);                 /* a directory's own entry */
/* a path from the partition's root, '/' between names: the directory's cluster */
int  ffs_resolve(struct ffs *v, const char *path, uint32_t *dir, struct ffs_ent *e);

int  ffs_create(struct ffs *v, uint32_t dir, const char *name, uint32_t len, int isdir, struct ffs_ent *e);
int  ffs_delete(struct ffs *v, const struct ffs_ent *e);
int  ffs_rename(struct ffs *v, const struct ffs_ent *e, uint32_t ndir, const char *name, uint32_t len, struct ffs_ent *out);
int  ffs_set_attr(struct ffs *v, struct ffs_ent *e, uint8_t attr);
int  ffs_set_date(struct ffs *v, struct ffs_ent *e, uint16_t date, uint16_t time);
int  ffs_name_ok(const char *name, uint32_t len);

/* files */
void ffs_open(struct ffs_file *h, const struct ffs_ent *e);
/* returns the bytes moved (>= 0) or an error; a short write sets *err */
int32_t ffs_read(struct ffs *v, struct ffs_file *h, uint32_t pos, void *buf, uint32_t n);
int32_t ffs_write(struct ffs *v, struct ffs_file *h, uint32_t pos, const void *buf, uint32_t n, int *err);
int  ffs_truncate(struct ffs *v, struct ffs_file *h, uint32_t size);
int  ffs_flush_file(struct ffs *v, struct ffs_file *h);          /* its entry (size, date) */

int  ffs_free_clusters(struct ffs *v, uint32_t *n);
uint32_t ffs_cluster_bytes(const struct ffs *v);

/* AmigaDOS dates (days since 1978, minutes, ticks) and FAT's */
void ffs_amiga_to_fat(uint32_t days, uint32_t mins, uint32_t ticks, uint16_t *date, uint16_t *time);
void ffs_fat_to_amiga(uint16_t date, uint16_t time, uint32_t *days, uint32_t *mins, uint32_t *ticks);
/* Latin-1, no case */
int  ffs_name_eq(const char *a, const char *b, uint32_t blen);
#endif

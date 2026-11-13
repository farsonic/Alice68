/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: the SD card's exFAT partition - the format Windows and macOS give
 * cards over 32 GB, with no 4 GB file limit. Written from Microsoft's exFAT
 * specification (published 2019). The Pi boots from the card's FAT32 partition;
 * a second partition in exFAT holds hardfiles (*.hdf, root and HDF) and CD
 * images (*.iso, root and ISO) as the FAT partition does.
 *
 * Host-independent like hdf_core.c (struct hdf_io, byte-by-byte little-endian
 * reads), so tools/hdf-test runs it on a PC against cards made by mkfs.exfat
 * and filled through exfat-fuse. 512-byte sectors only (what SD cards have).
 */
#ifndef ALICE68_EXFAT_H
#define ALICE68_EXFAT_H
#include "hdf_core.h"
#include "fatfs.h"

#define XFAT_NAME    108                /* Amiga names: up to 107 characters */
#define XFAT_FATWIN  8u                 /* FAT sectors cached */

#define XFAT_A_RO     0x01
#define XFAT_A_HIDDEN 0x02
#define XFAT_A_SYSTEM 0x04
#define XFAT_A_DIR    0x10
#define XFAT_A_ARCH   0x20

struct xfat {
    struct hdf_io *io;
    uint32_t part_start;            /* card sector of the partition */
    uint32_t fat_start;             /* card sector of the FAT in use (ActiveFat) */
    uint32_t heap_start;            /* card sector of cluster 2 */
    uint32_t spc_shift;             /* sectors per cluster: 1 << spc_shift */
    uint32_t clusters;              /* clusters in the heap */
    uint32_t root;                  /* the root directory's first cluster */
    uint16_t flags;                 /* VolumeFlags: 2 = dirty (not cleanly unmounted) */
    uint8_t *sec;                   /* one sector, for directories */
    uint32_t sec_no;                /* its card sector, ~0 = none */
    uint8_t *fwin;                  /* FAT window */
    uint32_t fwin_first;            /* ~0 = none */
    uint32_t wc_first, wc_ci, wc_c; /* directory walk: the chain from wc_first, its wc_ci-th cluster is wc_c */
    char     label[XFAT_NAME];      /* volume label, Latin-1 */
    uint32_t bm_first;              /* the Allocation Bitmap's first cluster, 0 = none found */
    uint64_t bm_len;                /* its bytes */
    /* for folders (the ffs_* calls, xfs_*): every directory seen, as exFAT has no ".." entries */
    struct xdir *dirs;
    uint32_t ndirs, capdirs;
    uint8_t *bounce;                /* one sector, for partial file reads */
    uint32_t nfree;                 /* free clusters, ~0 = not counted */
};

/* a directory the folder code has seen: where it is, and its parent */
struct xdir {
    uint32_t first;                 /* its first cluster: the directory's key */
    uint32_t parent;                /* its parent's key (the root's: itself) */
    uint32_t pos;                   /* its entry set's index in the parent */
    uint32_t nofat;
    uint64_t size;                  /* 0 for the root (follow the FAT chain) */
};

/* a directory: where its clusters are */
struct xfat_dir {
    uint32_t first;                 /* first cluster */
    uint64_t size;                  /* bytes; 0 = follow the FAT chain to its end (the root) */
    int      nofat;                 /* contiguous: no FAT chain */
};

/* an entry set (File + Stream Extension + File Name entries) */
struct xfat_ent {
    uint32_t pos;                   /* index of its File entry in the directory (32-byte units) */
    uint32_t count;                 /* entries in the set */
    uint16_t attr;
    uint32_t mtime;                 /* LastModifiedTimestamp: FAT date << 16 | time */
    uint32_t first;                 /* first cluster, 0 = none */
    uint64_t size;                  /* DataLength */
    uint64_t valid;                 /* ValidDataLength: beyond it the file reads as zeros */
    int      nofat;
    char     name[XFAT_NAME];       /* Latin-1, '?' for what it cannot hold */
};

/* the card's first exFAT partition: 0 opened, -1 none or unreadable (logged only if broken) */
int  xfat_open(struct xfat *v, struct hdf_io *io);
void xfat_close(struct xfat *v);
void xfat_root(const struct xfat *v, struct xfat_dir *d);
void xfat_ent_dir(const struct xfat_ent *e, struct xfat_dir *d);

/* the next entry set from *pos (0 = start); 0 ok, -1 end, -2 card error, -3 corrupt */
int  xfat_next(struct xfat *v, const struct xfat_dir *d, uint32_t *pos, struct xfat_ent *e);
/* a name in a directory, Latin-1 without case; 0 found, -1 not */
int  xfat_find(struct xfat *v, const struct xfat_dir *d, const char *name, uint32_t len, struct xfat_ent *e);

/* the card sectors of a file's first blocks (512 bytes each): extents into h, 0 ok */
int  xfat_extents(struct xfat *v, const struct xfat_ent *e, uint32_t blocks, struct hdf_file *h);
/* every *.hdf (iso = 0) or *.iso (iso = 1) in the root and in a root folder HDF / ISO,
   as fat_find_hdfs / fat_find_isos do for the FAT partition */
int  xfat_find_files(struct xfat *v, int iso, struct hdf_file *files, int max);

/* the ffs_* calls on an exFAT volume (fatfs.c hands them over when v->x is set). Directories are
   named by their first cluster, the root by v->x->root. Read-only so far: writes give FFS_WP. */
void xfs_done(struct ffs *v);
int  xfs_sync(struct ffs *v);
int  xfs_next(struct ffs *v, uint32_t dir, uint32_t *pos, struct ffs_ent *o);
int  xfs_find(struct ffs *v, uint32_t dir, const char *name, uint32_t len, struct ffs_ent *e);
int  xfs_parent(struct ffs *v, uint32_t dir, uint32_t *parent);
int  xfs_dir_ent(struct ffs *v, uint32_t dir, struct ffs_ent *e);
int  xfs_resolve(struct ffs *v, const char *path, uint32_t *dir, struct ffs_ent *e);
int  xfs_create(struct ffs *v, uint32_t dir, const char *name, uint32_t len, int isdir, struct ffs_ent *o);
int  xfs_delete(struct ffs *v, const struct ffs_ent *e);
int  xfs_rename(struct ffs *v, const struct ffs_ent *e, uint32_t ndir, const char *name, uint32_t len, struct ffs_ent *o);
int  xfs_set_attr(struct ffs *v, struct ffs_ent *e, uint8_t attr);
int  xfs_set_date(struct ffs *v, struct ffs_ent *e, uint16_t date, uint16_t time);
int  xfs_flush_file(struct ffs *v, struct ffs_file *h);
int32_t xfs_read(struct ffs *v, struct ffs_file *h, uint32_t pos, void *buf, uint32_t n);
int32_t xfs_write(struct ffs *v, struct ffs_file *h, uint32_t pos, const void *buf, uint32_t n, int *err);
int  xfs_truncate(struct ffs *v, struct ffs_file *h, uint32_t size);
int  xfs_free_clusters(struct ffs *v, uint32_t *n);
uint32_t xfs_cluster_bytes(const struct ffs *v);
uint32_t xfs_clusters(const struct ffs *v);
uint32_t xfs_clus_sec(const struct ffs *v, uint32_t c);
void xfs_set_io(struct ffs *v, struct hdf_io *io);
#endif

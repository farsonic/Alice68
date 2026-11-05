/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 hardfiles: the parts that do not depend on AmigaOS - the SD card's
 * FAT16/FAT32 boot partition, the *.hdf files on it and where their blocks
 * lie on the card, and the Rigid Disk Block inside a hardfile.
 *
 * Built twice: into alice68hdf.device (68k, big-endian) and natively on a PC
 * for tests (tools/hdf-test). All on-disk values are read byte by byte, so
 * the host's byte order does not matter.
 */
#ifndef ALICE68_HDF_CORE_H
#define ALICE68_HDF_CORE_H
#include <stdint.h>
#include <stdarg.h>

#define HDF_BLOCK       512u
#define HDF_MAX_FILES   16
#define HDF_MAX_PARTS   16
#define HDF_NAME        64

struct hdf_io {
    void *ctx;
    /* read count 512-byte sectors of the whole card, from sector; 0 = ok */
    int  (*read)(void *ctx, uint32_t sector, uint32_t count, void *buf);
    void *(*alloc)(void *ctx, uint32_t size);           /* zeroed */
    void (*free)(void *ctx, void *p, uint32_t size);
    void (*log)(void *ctx, const char *line);
};

struct hdf_extent { uint32_t file_blk, card_sec, count; };

struct hdf_file {
    char     name[HDF_NAME];        /* long name if there is one, else 8.3 */
    uint32_t size;                  /* bytes (FAT32: below 4 GB) */
    uint32_t blocks;                /* size / 512 */
    uint32_t next;                  /* extents used */
    uint32_t cap;                   /* extents allocated */
    struct hdf_extent *ext;
};

struct fat {
    struct hdf_io *io;
    uint32_t part_start;            /* card sector of the partition */
    uint32_t fat_start;             /* card sector of the first FAT */
    uint32_t data_start;            /* card sector of cluster 2 */
    uint32_t root_start, root_secs; /* FAT16 root directory */
    uint32_t root_clus;             /* FAT32 root directory */
    uint32_t spc;                   /* sectors per cluster */
    uint32_t clusters;              /* data clusters */
    int      fat32;
    uint8_t *buf;                   /* FAT window, FAT_WIN sectors */
    uint32_t buf_first;             /* first FAT sector in buf, ~0 = none */
    uint8_t *sec;                   /* one sector, for directories and the BPB */
};

struct hdf_part {
    char     name[32];              /* DOS device name, e.g. DH0 */
    uint32_t flags;                 /* PART pb_Flags: 1 bootable, 2 no-mount */
    uint32_t envec[20];             /* DosEnvec, de_TableSize first */
};

struct hdf_rdb {
    uint32_t cylinders, heads, sectors;
    int      nparts;
    struct hdf_part part[HDF_MAX_PARTS];
};

void hdf_fmt(char *out, unsigned size, const char *fmt, va_list ap);
void hdf_log(struct hdf_io *io, const char *fmt, ...);

int  fat_open(struct fat *f, struct hdf_io *io);
void fat_close(struct fat *f);
/* every *.hdf in the root directory and in a root directory named HDF;
   returns how many were put in files[] (each with its extents) */
int  fat_find_hdfs(struct fat *f, struct hdf_file *files, int max);
void hdf_file_free(struct hdf_io *io, struct hdf_file *h);

/* the card sector of a hardfile block, and how many blocks follow it
   contiguously; 0 = ok, -1 = outside the file */
int  hdf_map(const struct hdf_file *h, uint32_t blk, uint32_t *card_sec, uint32_t *run);
int  hdf_read(struct hdf_io *io, const struct hdf_file *h, uint32_t blk, uint32_t count, void *buf);

/* 1: a Rigid Disk Block was found and parsed; 0: none; -1: broken */
int  hdf_rdb(struct hdf_io *io, const struct hdf_file *h, struct hdf_rdb *r);

static inline uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
#endif

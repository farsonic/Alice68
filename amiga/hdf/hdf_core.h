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
    /* write; 0 = ok, -2 = the card is write-protected, -1 = failed (may be 0 when never used) */
    int  (*write)(void *ctx, uint32_t sector, uint32_t count, const void *buf);
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
    uint32_t cmd_clus, cmd_size;    /* cmdline.txt in the root directory, if any */
    uint32_t fat_secs, nfats;       /* one FAT's sectors; how many FATs (fatfs.c writes them all) */
    uint32_t fsinfo;                /* FAT32: card sector of FSInfo, 0 = none */
    uint32_t cfg_clus, cfg_size;    /* config.txt in the root directory, if any */
};

struct hdf_part {
    char     name[32];              /* DOS device name, e.g. DH0 */
    uint32_t flags;                 /* PART pb_Flags: 1 bootable, 2 no-mount */
    uint32_t envec[20];             /* DosEnvec, de_TableSize first */
};

/* options from cmdline.txt */
struct hdf_opts {
    int  off;                       /* hdf=off */
    int  readonly;                  /* hdf.readonly */
    int  noboot;                    /* hdf.noboot: mount every hardfile, boot from none (but hdf.boot) */
    char boot[HDF_NAME];            /* hdf.boot=NAME: this hardfile boots and wins name clashes */
};

struct hdf_rdb {
    uint32_t cylinders, heads, sectors;
    uint32_t fshd;                  /* first FileSysHeaderBlock, ~0 = none */
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
/* the WiFi firmware files: everything in a WIFI folder, and cyfmac4345x* / brcmfmac4345x* in the root */
int  fat_find_wifi(struct fat *f, struct hdf_file *files, int max);
void hdf_file_free(struct hdf_io *io, struct hdf_file *h);

/* the card sector of a hardfile block, and how many blocks follow it
   contiguously; 0 = ok, -1 = outside the file */
int  hdf_map(const struct hdf_file *h, uint32_t blk, uint32_t *card_sec, uint32_t *run);
int  hdf_read(struct hdf_io *io, const struct hdf_file *h, uint32_t blk, uint32_t count, void *buf);
/* 0 ok, -1 outside the file or failed, -2 write-protected card */
int  hdf_write(struct hdf_io *io, const struct hdf_file *h, uint32_t blk, uint32_t count, const void *buf);

/* cmdline.txt from the root directory (after fat_find_hdfs), NUL-terminated;
   returns its length, or -1 if there is none */
int  fat_read_cmdline(struct fat *f, char *buf, uint32_t max);
void hdf_parse_opts(const char *cmdline, struct hdf_opts *o);
/* a file name as given in cmdline.txt: no case, and '_' stands for ' ' */
int  hdf_name_match(const char *file, const char *want);
/* a single-partition hardfile (a filesystem from block 0, as WinUAE makes
   them): 1 found, with one nameless partition in r; 0 not one */
int  hdf_single(struct hdf_io *io, const struct hdf_file *h, struct hdf_rdb *r);

/* a filesystem stored in the RDB: its FileSysHeaderBlock */
struct hdf_fs {
    uint32_t dostype, version, patchflags;
    uint32_t dn[9];                 /* dn_Type .. dn_GlobalVec as the header gives them (dn[7]: the first LSEG block) */
};
/* the filesystems in an RDB; returns how many (up to max), -1 broken list */
int  hdf_rdb_fslist(struct hdf_io *io, const struct hdf_file *h, const struct hdf_rdb *r, struct hdf_fs *fs, int max);
/* the load file of one: its LSEG blocks' data, in a buffer from io->alloc
   (*size bytes, allocated as *alloc bytes); 0 ok */
int  hdf_rdb_fsdata(struct hdf_io *io, const struct hdf_file *h, const struct hdf_fs *fs, uint8_t **data, uint32_t *size, uint32_t *alloc);

/* the volume name of a partition with FFS (DOS\0..7), from its root block:
   1 found, 0 not FFS or no root block there */
int  hdf_volume(struct hdf_io *io, const struct hdf_file *h, const struct hdf_part *p, char name[32]);

/* the card's own Amiga partitions (MBR type 0x76), each as a one-piece
   "file", so that hdf_rdb() and hdf_volume() work on them; returns how many */
int  card_amiga_parts(struct hdf_io *io, struct hdf_file *out, int max);

/* 1: a Rigid Disk Block was found and parsed; 0: none; -1: broken */
int  hdf_rdb(struct hdf_io *io, const struct hdf_file *h, struct hdf_rdb *r);

static inline uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
#endif

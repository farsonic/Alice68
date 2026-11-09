/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 CD images: reading ISO 9660 images (*.iso on the SD card's FAT
 * partition), for the read-only volumes alice68hdf.device mounts (isos.c).
 * Names: Rock Ridge if the image has it, else Joliet, else ISO 9660's own
 * (without ";1"). Latin-1 on the Amiga side.
 *
 * Host-independent like hdf_core.c and fatfs.c: the image is a struct
 * hdf_file (its extents on the card) read through struct hdf_io, so
 * tools/hdf-test runs this on a PC against images made by genisoimage.
 * One caller per struct iso at a time (each volume's own handler process).
 */
#ifndef ALICE68_ISO9660_H
#define ALICE68_ISO9660_H
#include "hdf_core.h"

#define ISO_SEC      2048u              /* logical sector */
#define ISO_NAME     108                /* Amiga names: up to 107 characters */

enum {
    ISO_OK = 0,
    ISO_END = -1,                       /* no more entries / not found */
    ISO_IO = -2,
    ISO_NOTISO = -3,                    /* no ISO 9660 volume descriptor */
    ISO_CORRUPT = -4,
    ISO_NOTDIR = -5,
};

/* an entry: directories are named by their first sector (extent) */
struct iso_ent {
    uint32_t extent, size;              /* first sector, bytes */
    uint32_t dir;                       /* the directory it is in (its extent) */
    uint32_t pos;                       /* its record's byte offset there: with dir, a file's identity */
    uint8_t  isdir, hidden;
    uint8_t  date[7];                   /* ISO 9660 recording date */
    char     name[ISO_NAME];
};

struct iso {
    struct hdf_io *io;
    const struct hdf_file *f;
    uint32_t blocks;                    /* the volume's size, sectors */
    struct iso_ent root;
    int      joliet;                    /* names from the Joliet tree */
    int      rr;                        /* Rock Ridge names (SUSP), with rr_skip bytes before them */
    uint32_t rr_skip;
    char     vol[33];                   /* volume label, trimmed */
    uint8_t *buf;                       /* one sector of a directory */
    uint32_t buf_sec;                   /* which, ~0 = none */
    uint8_t *ce;                        /* a Rock Ridge continuation area's sector */
    uint8_t *bounce;                    /* one 512-byte block for partial reads */
};

/* the volume descriptors: 0 ok, else ISO_NOTISO / ISO_IO */
int  iso_open(struct iso *v, struct hdf_io *io, const struct hdf_file *f);
void iso_close(struct iso *v);
/* a directory's next entry from *pos (0 = start; "." and ".." are skipped) */
int  iso_next(struct iso *v, const struct iso_ent *dir, uint32_t *pos, struct iso_ent *e);
/* a name in a directory, Latin-1 without case */
int  iso_find(struct iso *v, const struct iso_ent *dir, const char *name, uint32_t len, struct iso_ent *e);
/* the directory an entry is in (the root's is the root): its own entry */
int  iso_parent(struct iso *v, const struct iso_ent *e, struct iso_ent *parent);
/* bytes read (>= 0) or an error */
int32_t iso_read(struct iso *v, const struct iso_ent *e, uint32_t pos, void *buf, uint32_t n);
/* an ISO 9660 date as AmigaDOS's (days since 1978, minutes, ticks) */
void iso_date(const uint8_t d[7], uint32_t *days, uint32_t *mins, uint32_t *ticks);
int  iso_name_eq(const char *a, const char *b, uint32_t blen);
#endif

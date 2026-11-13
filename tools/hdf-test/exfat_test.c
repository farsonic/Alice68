/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Host test of amiga/hdf/exfat.c: opens a card image's exFAT partition as
 * alice68hdf.device would and prints what it finds, for exfat.sh to compare:
 *   V label
 *   H name blocks pieces crc32    (hardfiles; crc of every block, read through the extents)
 *   I name blocks pieces crc32    (CD images)
 *   B name blocks pieces markers  (a file over 64 MB: only its marker blocks are read)
 *   L name                        (each entry of the root, then of the folder HDF)
 *   N name size                   (xfat_find of the names given after the image, another case)
 *     exfat_test IMAGE [NAME...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exfat.h"

static FILE *img;
static int rd(void *ctx, uint32_t sec, uint32_t n, void *buf)
{
    (void)ctx;
    if (fseeko(img, (off_t)sec * 512, SEEK_SET)) return -1;
    return fread(buf, 512, n, img) == n ? 0 : -1;
}
static void *al(void *ctx, uint32_t n) { (void)ctx; return calloc(1, n); }
static void fr(void *ctx, void *p, uint32_t n) { (void)ctx; (void)n; free(p); }
static void lg(void *ctx, const char *l) { (void)ctx; fprintf(stderr, "log: %s\n", l); }

static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n)
{
    c = ~c;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}

static void show(struct hdf_io *io, const char *kind, struct hdf_file *h)
{
    static uint8_t buf[512 * 256];
    if (h->blocks > 131072) {                                /* the big one: its marker blocks */
        uint32_t at[4] = { 0, 8388607, 8388608, h->blocks - 1 }, ok = 0;
        for (int i = 0; i < 4; i++) {
            char want[32];
            snprintf(want, sizeof want, "BIG BLOCK %u", at[i]);
            if (!hdf_read(io, h, at[i], 1, buf) && !memcmp(buf, want, strlen(want) + 1)) ok++;
        }
        printf("B %s %u %u %u\n", h->name, h->blocks, h->next, ok);
        return;
    }
    uint32_t c = 0;
    for (uint32_t b = 0; b < h->blocks; b += 256) {
        uint32_t n = h->blocks - b < 256 ? h->blocks - b : 256;
        if (hdf_read(io, h, b, n, buf)) { printf("FAIL %s: read at %u\n", h->name, b); return; }
        c = crc32(c, buf, n * 512);
    }
    printf("%s %s %u %u %08x\n", kind, h->name, h->blocks, h->next, c);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !(img = fopen(argv[1], "rb"))) return 2;
    struct hdf_io io = { 0, rd, 0, al, fr, lg };
    struct xfat v;
    if (xfat_open(&v, &io)) { printf("NOEXFAT\n"); return 1; }
    printf("V %s\n", v.label);
    struct hdf_file files[16];
    int n = xfat_find_files(&v, 0, files, 16);
    for (int i = 0; i < n; i++) { show(&io, "H", &files[i]); hdf_file_free(&io, &files[i]); }
    n = xfat_find_files(&v, 1, files, 16);
    for (int i = 0; i < n; i++) { show(&io, "I", &files[i]); hdf_file_free(&io, &files[i]); }
    struct xfat_dir d, sub;
    struct xfat_ent e;
    xfat_root(&v, &d);
    uint32_t pos = 0;
    while (xfat_next(&v, &d, &pos, &e) == 0) printf("L %s\n", e.name);
    if (xfat_find(&v, &d, "hdf", 3, &e) == 0) {
        xfat_ent_dir(&e, &sub);
        pos = 0;
        while (xfat_next(&v, &sub, &pos, &e) == 0) printf("L HDF/%s\n", e.name);
    }
    for (int i = 2; i < argc; i++)
        printf("N %s %lld\n", argv[i], xfat_find(&v, &d, argv[i], (uint32_t)strlen(argv[i]), &e) ? -1LL : (long long)e.size);
    xfat_close(&v);
    return 0;
}

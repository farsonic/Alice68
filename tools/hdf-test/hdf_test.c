/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of amiga/hdf/hdf_core.c against a card image from mkimg.py:
 * finds the hardfiles, compares every block of each with the original file,
 * and prints what the RDB says.  hdf_test CARD.img DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdf_core.h"

static FILE *card;
static unsigned long reads;

static int rd(void *ctx, uint32_t sector, uint32_t count, void *buf)
{
    (void)ctx;
    reads++;
    if (fseek(card, (long)sector * 512, SEEK_SET)) return -1;
    return fread(buf, 512, count, card) == count ? 0 : -1;
}
static void *al(void *ctx, uint32_t n) { (void)ctx; return calloc(1, n); }
static void fr(void *ctx, void *p, uint32_t n) { (void)ctx; (void)n; free(p); }
static void lg(void *ctx, const char *l) { (void)ctx; printf("  log: %s\n", l); }

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    card = fopen(argv[1], "rb");
    if (!card) return 3;
    struct hdf_io io = { 0, rd, al, fr, lg };
    struct fat f;
    if (fat_open(&f, &io)) { printf("FAIL: fat_open\n"); return 1; }
    struct hdf_file files[HDF_MAX_FILES];
    int n = fat_find_hdfs(&f, files, HDF_MAX_FILES);
    int fails = 0;
    static const char *const want[] = { "games.hdf", "Workbench 3.1.hdf", "raw.hdf", "frag.hdf", "extra.hdf" };
    if (n != 5) { printf("FAIL: %d hardfiles, want 5\n", n); fails++; }
    for (int w = 0; w < 5; w++) {
        int seen = 0;
        for (int i = 0; i < n; i++) seen += !strcmp(files[i].name, want[w]);
        if (seen != 1) { printf("FAIL: '%s' found %d times\n", want[w], seen); fails++; }
    }
    if (n == 5 && strcmp(files[4].name, "extra.hdf")) { printf("FAIL: the HDF folder is not scanned after the root\n"); fails++; }
    for (int i = 0; i < n; i++) {
        struct hdf_file *h = &files[i];
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", argv[2], h->name);
        FILE *o = fopen(path, "rb");
        if (!o) { printf("FAIL: no original %s\n", path); fails++; continue; }
        uint8_t a[512 * 8], b[512 * 8];
        unsigned long bad = 0;
        for (uint32_t blk = 0; blk < h->blocks; blk += 8) {
            uint32_t c = h->blocks - blk < 8 ? h->blocks - blk : 8;
            if (hdf_read(&io, h, blk, c, a)) { bad++; break; }
            if (fread(b, 512, c, o) != c || memcmp(a, b, 512 * c)) bad++;
        }
        uint32_t s, r;
        if (hdf_map(h, h->blocks, &s, &r) == 0) { printf("FAIL: %s: block past the end mapped\n", h->name); bad++; }
        fclose(o);
        printf("%s: %u blocks, %u extents, %s\n", h->name, h->blocks, h->next, bad ? "MISMATCH" : "all blocks match");
        fails += bad != 0;
        struct hdf_rdb rdb;
        int k = hdf_rdb(&io, h, &rdb);
        if (k == 1) {
            printf("  RDB: %u cyl, %u heads, %u sectors, %d partition(s)\n", rdb.cylinders, rdb.heads, rdb.sectors, rdb.nparts);
            for (int p = 0; p < rdb.nparts; p++)
                printf("    %s: cyl %u-%u, boot %s pri %d, dostype %08x\n", rdb.part[p].name, rdb.part[p].envec[9],
                       rdb.part[p].envec[10], rdb.part[p].flags & 1 ? "yes" : "no", (int)rdb.part[p].envec[15], rdb.part[p].envec[16]);
        } else {
            printf("  no RDB (%d)\n", k);
        }
        if (!strcmp(h->name, "frag.hdf") && h->next < 17) { printf("FAIL: frag.hdf only %u pieces\n", h->next); fails++; }
        if (!strcmp(h->name, "raw.hdf") && k != 0) { printf("FAIL: raw.hdf has no RDB\n"); fails++; }
        if (!strcmp(h->name, "games.hdf") && (k != 1 || rdb.nparts != 2 || strcmp(rdb.part[1].name, "GAMES") ||
            (int)rdb.part[1].envec[15] != -5 || h->next < 2)) { printf("FAIL: games.hdf RDB/fragmentation\n"); fails++; }
        hdf_file_free(&io, h);
    }
    fat_close(&f);
    printf("%lu card reads\n%s\n", reads, fails ? "FAILED" : "ALL OK");
    return fails != 0;
}

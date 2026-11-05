/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of amiga/hdf/hdf_core.c against a card image from mkimg.py:
 * finds the hardfiles, compares every block of each with the original file,
 * checks the RDBs, the single-partition hardfile, cmdline.txt and its
 * options, then writes through the mapping into a copy of the card and
 * checks that exactly the written blocks changed.  hdf_test CARD.img DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdf_core.h"

static FILE *card;
static unsigned long reads, writes;

static int rd(void *ctx, uint32_t sector, uint32_t count, void *buf)
{
    (void)ctx;
    reads++;
    if (fseek(card, (long)sector * 512, SEEK_SET)) return -1;
    return fread(buf, 512, count, card) == count ? 0 : -1;
}
static int wr(void *ctx, uint32_t sector, uint32_t count, const void *buf)
{
    (void)ctx;
    writes++;
    if (fseek(card, (long)sector * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, count, card) == count ? 0 : -1;
}
static void *al(void *ctx, uint32_t n) { (void)ctx; return calloc(1, n); }
static void fr(void *ctx, void *p, uint32_t n) { (void)ctx; (void)n; free(p); }
static void lg(void *ctx, const char *l) { (void)ctx; printf("  log: %s\n", l); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return 0; }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    /* work on a copy: the write test changes it */
    long cn;
    uint8_t *orig = slurp(argv[1], &cn);
    if (!orig) return 3;
    card = tmpfile();
    if (!card || fwrite(orig, 1, (size_t)cn, card) != (size_t)cn) return 3;

    struct hdf_io io = { 0, rd, wr, al, fr, lg };
    struct fat f;
    if (fat_open(&f, &io)) { printf("FAIL: fat_open\n"); return 1; }
    struct hdf_file files[HDF_MAX_FILES];
    int n = fat_find_hdfs(&f, files, HDF_MAX_FILES);
    char cmd[1024];
    int cl = fat_read_cmdline(&f, cmd, sizeof cmd);
    fat_close(&f);
    CHECK(cl > 0 && (strstr(cmd, "aga.look") || strstr(cmd, "hdf")), "cmdline.txt not read (%d)", cl);
    struct hdf_opts o;
    hdf_parse_opts(cmd, &o);
    if (!strstr(cmd, "hdf")) CHECK(!o.off && !o.readonly && !o.boot[0], "default cmdline.txt parsed as options");
    else CHECK(o.off || o.readonly || o.noboot || o.boot[0], "cmdline.txt options not parsed: %s", cmd);
    hdf_parse_opts("aga.look=0  hdf.boot=Workbench_3.1.hdf\thdf.readonly\n", &o);
    CHECK(o.readonly && !strcmp(o.boot, "Workbench_3.1.hdf"), "options parse");
    CHECK(hdf_name_match("Workbench 3.1.hdf", o.boot) && !hdf_name_match("Workbench 3.1.hdf", "workbench"), "name match");
    hdf_parse_opts("hdf=off", &o);
    CHECK(o.off, "hdf=off");

    static const char *const want[] = { "games.hdf", "Workbench 3.1.hdf", "raw.hdf", "frag.hdf", "extra.hdf" };
    CHECK(n == 5, "%d hardfiles, want 5", n);
    for (int w = 0; w < 5; w++) {
        int seen = 0;
        for (int i = 0; i < n; i++) seen += !strcmp(files[i].name, want[w]);
        CHECK(seen == 1, "'%s' found %d times", want[w], seen);
    }
    CHECK(n == 5 && !strcmp(files[4].name, "extra.hdf"), "the HDF folder is not scanned after the root");
    struct hdf_file *wb = 0, *fragf = 0;
    for (int i = 0; i < n; i++) {
        struct hdf_file *h = &files[i];
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", argv[2], h->name);
        FILE *of = fopen(path, "rb");
        if (!of) { CHECK(0, "no original %s", path); continue; }
        uint8_t a[512 * 8], b[512 * 8];
        unsigned long bad = 0;
        for (uint32_t blk = 0; blk < h->blocks; blk += 8) {
            uint32_t c = h->blocks - blk < 8 ? h->blocks - blk : 8;
            if (hdf_read(&io, h, blk, c, a)) { bad++; break; }
            if (fread(b, 512, c, of) != c || memcmp(a, b, 512 * c)) bad++;
        }
        uint32_t s, r;
        CHECK(hdf_map(h, h->blocks, &s, &r) != 0, "%s: block past the end mapped", h->name);
        fclose(of);
        printf("%s: %u blocks, %u extents, %s\n", h->name, h->blocks, h->next, bad ? "MISMATCH" : "all blocks match");
        CHECK(!bad, "%s: blocks differ", h->name);
        struct hdf_rdb rdb;
        int k = hdf_rdb(&io, h, &rdb);
        if (!strcmp(h->name, "raw.hdf")) {
            CHECK(k == 0, "raw.hdf has no RDB");
            int sk = hdf_single(&io, h, &rdb);
            CHECK(sk == 1 && rdb.heads == 1 && rdb.sectors == 32 && rdb.cylinders == 64 && rdb.part[0].envec[16] == 0x444f5301u &&
                  rdb.part[0].envec[6] == 2 && rdb.part[0].envec[10] == 63, "raw.hdf single-partition geometry");
            printf("  single partition: %u cyl, %u heads, %u sectors\n", rdb.cylinders, rdb.heads, rdb.sectors);
        } else {
            CHECK(k == 1, "%s: no RDB", h->name);
            CHECK(hdf_single(&io, h, &rdb) == 0, "%s: an RDB hardfile taken for a single-partition one", h->name);
        }
        if (!strcmp(h->name, "games.hdf")) {
            hdf_rdb(&io, h, &rdb);
            CHECK(rdb.nparts == 2 && !strcmp(rdb.part[1].name, "GAMES") && (int)rdb.part[1].envec[15] == -5 && h->next >= 2,
                  "games.hdf RDB/fragmentation");
        }
        if (!strcmp(h->name, "frag.hdf")) { CHECK(h->next >= 17, "frag.hdf only %u pieces", h->next); fragf = h; }
        if (!strcmp(h->name, "Workbench 3.1.hdf")) wb = h;
    }

    /* writes: 16 blocks into Workbench, 400 blocks across frag.hdf's pieces */
    if (wb && fragf) {
        uint8_t *buf = malloc(512 * 400);
        for (int i = 0; i < 512 * 400; i++) buf[i] = (uint8_t)(0xa5 ^ (i * 7) ^ (i >> 9));
        CHECK(hdf_write(&io, wb, 2000, 16, buf) == 0, "write Workbench");
        CHECK(hdf_write(&io, fragf, 50, 400, buf) == 0, "write frag");
        CHECK(hdf_write(&io, wb, wb->blocks - 1, 2, buf) != 0, "a write past the end was not refused");
        uint8_t *back = malloc(512 * 400);
        CHECK(hdf_read(&io, wb, 2000, 16, back) == 0 && !memcmp(back, buf, 512 * 16), "Workbench read-back");
        CHECK(hdf_read(&io, fragf, 50, 400, back) == 0 && !memcmp(back, buf, 512 * 400), "frag read-back");
        /* nothing else on the card changed: exactly the 416 written sectors */
        fflush(card);
        long changed = 0;
        uint8_t sec[512];
        for (long s = 0; s < cn / 512; s++) {
            fseek(card, s * 512, SEEK_SET);
            if (fread(sec, 1, 512, card) != 512) { changed = -1; break; }
            if (memcmp(sec, orig + s * 512, 512)) changed++;
        }
        CHECK(changed == 416, "%ld sectors changed on the card, want 416", changed);
        printf("writes: %ld sectors changed on the card (want 416), read back OK\n", changed);
        free(buf); free(back);
    }
    for (int i = 0; i < n; i++) hdf_file_free(&io, &files[i]);
    printf("%lu card reads, %lu writes\n%s\n", reads, writes, fails ? "FAILED" : "ALL OK");
    return fails != 0;
}

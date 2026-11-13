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
 * and with -w, the folder engine (the ffs_* calls on exFAT, as folders.c makes them):
 *   D path / F path size crc32    (every entry; files read in 777-byte pieces)
 *   S free-clusters cluster-bytes
 *     exfat_test -w IMAGE
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

static int fails;
static void walk(struct ffs *v, uint32_t dir, const char *path, int depth)
{
    static uint8_t buf[1 << 16];
    uint32_t pos = 0;
    struct ffs_ent e;
    int r;
    if (depth > 40) { printf("FAIL %s: too deep\n", path); fails++; return; }
    while ((r = ffs_next(v, dir, &pos, &e)) == 0) {
        char p[4096];
        snprintf(p, sizeof p, "%s%s%s", path, path[0] ? "/" : "", e.name);
        struct ffs_ent f;
        if (ffs_find(v, dir, e.name, (uint32_t)strlen(e.name), &f) || f.sec != e.sec || f.slot != e.slot) { printf("FAIL %s: ffs_find\n", p); fails++; }
        if (e.attr & FFS_A_DIR) {
            uint32_t par;
            struct ffs_ent self;
            if (ffs_parent(v, e.first, &par) || par != dir) { printf("FAIL %s: parent\n", p); fails++; }
            if (ffs_dir_ent(v, e.first, &self) || self.first != e.first || strcmp(self.name, e.name)) { printf("FAIL %s: dir_ent\n", p); fails++; }
            printf("D %s\n", p);
            walk(v, e.first, p, depth + 1);
        } else if (e.xsize > (64u << 20)) {
            printf("F %s %llu -\n", p, (unsigned long long)e.xsize);
        } else {
            struct ffs_file h;
            ffs_open(&h, &e);
            uint32_t c = 0, at = 0;
            int32_t n;
            while ((n = ffs_read(v, &h, at, buf + (at & 3) + 1, 777)) > 0) { c = crc32(c, buf + (at & 3) + 1, (uint32_t)n); at += (uint32_t)n; }
            if (n < 0 || at != e.size) { printf("FAIL %s: read %d at %u\n", p, n, at); fails++; }
            printf("F %s %u %08x\n", p, e.size, c);
        }
    }
    if (r != FFS_END) { printf("FAIL %s: ffs_next %d\n", path, r); fails++; }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-w")) {
        if (!(img = fopen(argv[2], "rb"))) return 2;
        struct hdf_io io = { 0, rd, 0, al, fr, lg };
        struct xfat x;
        struct ffs v;
        if (xfat_open(&x, &io) || ffs_init_exfat(&v, &x)) { printf("NOEXFAT\n"); return 1; }
        walk(&v, x.root, "", 0);
        uint32_t fr_ = 0;
        if (ffs_free_clusters(&v, &fr_)) { printf("FAIL free clusters\n"); fails++; }
        printf("S %u %u\n", fr_, ffs_cluster_bytes(&v));
        struct ffs_ent e;
        uint32_t d;
        if (ffs_resolve(&v, "a/b/c", &d, &e) || strcmp(e.name, "c")) { printf("FAIL resolve a/b/c\n"); fails++; }
        if (ffs_resolve(&v, "Many/x", &d, &e) != FFS_END) { printf("FAIL resolve a missing path\n"); fails++; }
        struct ffs_file h;
        int err;
        ffs_open(&h, &e);
        if (ffs_create(&v, x.root, "new", 3, 0, &e) != FFS_WP || ffs_write(&v, &h, 0, "x", 1, &err) != 0 || err != FFS_WP) { printf("FAIL writes not refused\n"); fails++; }
        xfat_close(&x);
        return fails != 0;
    }
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

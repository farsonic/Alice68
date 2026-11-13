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
 * and with -W, writing (the image is changed): a fixed sequence of creates, writes, renames,
 * deletes, truncates through the ffs_* calls; prints E path size crc32 for every file it
 * leaves, X path for every name it removed, then "DONE writes"; with a budget the card
 * "loses power" after that many sector writes (they fail from then on):
 *     exfat_test -W IMAGE [BUDGET]
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
static long budget = -1, nwrites;
static int wr(void *ctx, uint32_t sec, uint32_t n, const void *buf)
{
    (void)ctx;
    if (budget >= 0 && nwrites >= budget) return -1;          /* the power is off */
    nwrites++;
    if (fseeko(img, (off_t)sec * 512, SEEK_SET)) return -1;
    return fwrite(buf, 512, n, img) == n ? 0 : -1;
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

/* ---- -W: writing --------------------------------------------------------------------------- */
static struct ffs W;
static uint32_t wroot;
static uint8_t pat[1 << 20];
static void fill(uint32_t seed) { for (uint32_t i = 0; i < sizeof pat; i++) { seed = seed * 1103515245u + 12345u; pat[i] = (uint8_t)(seed >> 16); } }
static int dir_of(const char *path, uint32_t *d, const char **leaf)
{
    char tmp[512];
    const char *sl = strrchr(path, '/');
    *leaf = sl ? sl + 1 : path;
    if (!sl) { *d = wroot; return 0; }
    snprintf(tmp, sizeof tmp, "%.*s", (int)(sl - path), path);
    struct ffs_ent e;
    return ffs_resolve(&W, tmp, d, &e);
}
static int look(const char *path, struct ffs_ent *e)
{
    uint32_t d; const char *leaf;
    if (dir_of(path, &d, &leaf)) return -1;
    return ffs_find(&W, d, leaf, (uint32_t)strlen(leaf), e);
}
static int mk(const char *path, int isdir)
{
    uint32_t d; const char *leaf; struct ffs_ent e;
    if (dir_of(path, &d, &leaf)) return -1;
    int r = ffs_create(&W, d, leaf, (uint32_t)strlen(leaf), isdir, &e);
    ffs_sync(&W);
    return r;
}
/* write len bytes of pat (from seed) at pos, in pieces of chunk */
static int put(const char *path, uint32_t pos, uint32_t len, uint32_t chunk, uint32_t seed)
{
    struct ffs_ent e;
    if (look(path, &e)) return -1;
    struct ffs_file h;
    ffs_open(&h, &e);
    fill(seed);
    for (uint32_t at = 0; at < len; at += chunk) {
        uint32_t n = len - at < chunk ? len - at : chunk;
        int err;
        if (ffs_write(&W, &h, pos + at, pat + at, n, &err) != (int32_t)n) return -1;
    }
    int r = ffs_flush_file(&W, &h);
    ffs_sync(&W);
    return r;
}
static int trunc_(const char *path, uint32_t size)
{
    struct ffs_ent e;
    if (look(path, &e)) return -1;
    struct ffs_file h;
    ffs_open(&h, &e);
    int r = ffs_truncate(&W, &h, size);
    if (!r) r = ffs_flush_file(&W, &h);
    ffs_sync(&W);
    return r;
}
static int mv(const char *from, const char *to)
{
    struct ffs_ent e, o;
    uint32_t d; const char *leaf;
    if (look(from, &e) || dir_of(to, &d, &leaf)) return -1;
    int r = ffs_rename(&W, &e, d, leaf, (uint32_t)strlen(leaf), &o);
    ffs_sync(&W);
    return r;
}
static int rm(const char *path)
{
    struct ffs_ent e;
    if (look(path, &e)) return -1;
    int r = ffs_delete(&W, &e);
    ffs_sync(&W);
    return r;
}
static void expect(const char *path)
{
    struct ffs_ent e;
    if (look(path, &e)) { printf("FAIL %s: not found\n", path); fails++; return; }
    struct ffs_file h;
    ffs_open(&h, &e);
    static uint8_t buf[1 << 16];
    uint32_t c = 0, at = 0;
    int32_t n;
    while ((n = ffs_read(&W, &h, at, buf, sizeof buf)) > 0) { c = crc32(c, buf, (uint32_t)n); at += (uint32_t)n; }
    printf("E %s %u %08x\n", path, e.size, c);
}
#define STEP(x) do { int r_ = (x); if (r_ && budget < 0) { printf("FAIL %s: %d\n", #x, r_); fails++; } } while (0)
static int write_test(void)
{
    char p[256];
    STEP(mk("New Dir", 1));
    STEP(mk("New Dir/small.txt", 0)); STEP(put("New Dir/small.txt", 0, 100, 100, 1));
    STEP(mk("New Dir/big.bin", 0)); STEP(put("New Dir/big.bin", 0, 300000, 7777, 2));
    STEP(mk("New Dir/A name of ninety characters, which needs six File Name entries in exFAT, x.txt", 0));
    STEP(put("New Dir/A name of ninety characters, which needs six File Name entries in exFAT, x.txt", 0, 5000, 5000, 3));
    STEP(mk("New Dir/Gr\xfc\xdf" "e \xe4\xf6\xff.txt", 0)); STEP(put("New Dir/Gr\xfc\xdf" "e \xe4\xf6\xff.txt", 0, 10, 10, 4));
    STEP(mk("New Dir/empty", 0));
    /* two files growing in turns: contiguous, then a FAT chain */
    STEP(mk("New Dir/turn A", 0)); STEP(mk("New Dir/turn B", 0));
    for (int i = 0; i < 20; i++) { STEP(put("New Dir/turn A", (uint32_t)i * 4096, 4096, 4096, 100 + (uint32_t)i)); STEP(put("New Dir/turn B", (uint32_t)i * 4096, 4096, 4096, 200 + (uint32_t)i)); }
    /* a directory that has to grow */
    STEP(mk("Lots", 1));
    for (int i = 0; i < 200; i++) { snprintf(p, sizeof p, "Lots/new file %03d.dat", i); STEP(mk(p, 0)); if (i % 10 == 0) STEP(put(p, 0, (uint32_t)i * 37 + 1, 999, 300 + (uint32_t)i)); }
    /* renames */
    STEP(mv("New Dir/small.txt", "New Dir/a much longer name than before for small.txt"));
    STEP(mv("New Dir/big.bin", "Lots/big moved.bin"));
    STEP(mk("New Dir/Sub", 1)); STEP(mk("New Dir/Sub/inner.txt", 0)); STEP(put("New Dir/Sub/inner.txt", 0, 1234, 1234, 5));
    STEP(mv("New Dir/Sub", "Lots/Sub moved"));
    if (budget < 0 && mv("Lots", "Lots/Sub moved/Lots") != FFS_LOOP) { printf("FAIL a folder into itself\n"); fails++; }
    if (budget < 0 && mk("Lots/new file 001.dat", 0) != FFS_EXISTS) { printf("FAIL create an existing name\n"); fails++; }
    if (budget < 0 && mk("New Dir/bad:name", 0) != FFS_BADNAME) { printf("FAIL a name exFAT cannot hold\n"); fails++; }
    /* deletes */
    for (int i = 0; i < 200; i += 3) { snprintf(p, sizeof p, "Lots/new file %03d.dat", i); STEP(rm(p)); }
    if (budget < 0 && rm("Lots") != FFS_NOTEMPTY) { printf("FAIL delete a folder that is not empty\n"); fails++; }
    STEP(mk("Gone", 1)); STEP(rm("Gone"));
    /* sizes */
    STEP(trunc_("New Dir/turn A", 30000));                     /* a FAT chain, shorter */
    STEP(trunc_("New Dir/turn B", 0));
    STEP(trunc_("New Dir/empty", 9000));                       /* longer: zeros (past ValidDataLength) */
    STEP(put("a/b/c/note \xe9t\xe9.txt", 100, 200, 200, 6));  /* inside a file the PC made */
    STEP(put("Many/zz last.txt", 70000, 5000, 5000, 7));       /* past its end */
    /* attributes, dates */
    struct ffs_ent e;
    if (!look("New Dir/empty", &e)) { STEP(ffs_set_attr(&W, &e, FFS_A_RO | FFS_A_ARCH)); STEP(ffs_set_date(&W, &e, (uint16_t)((45 << 9) | (6 << 5) | 15), (uint16_t)((12 << 11) | (34 << 5) | 28))); ffs_sync(&W); }
    if (budget >= 0) return 0;
    const char *left[] = { "New Dir/a much longer name than before for small.txt", "Lots/big moved.bin",
        "New Dir/A name of ninety characters, which needs six File Name entries in exFAT, x.txt", "New Dir/Gr\xfc\xdf" "e \xe4\xf6\xff.txt",
        "New Dir/empty", "New Dir/turn A", "New Dir/turn B", "Lots/Sub moved/inner.txt", "a/b/c/note \xe9t\xe9.txt", "Many/zz last.txt", 0 };
    for (int i = 0; left[i]; i++) expect(left[i]);
    for (int i = 0; i < 200; i++) { snprintf(p, sizeof p, "Lots/new file %03d.dat", i); if (i % 3) expect(p); else printf("X %s\n", p); }
    printf("X New Dir/small.txt\nX New Dir/big.bin\nX New Dir/Sub\nX Gone\n");
    uint32_t fr_;
    ffs_free_clusters(&W, &fr_);
    printf("S %u\n", fr_);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-W")) {
        if (!(img = fopen(argv[2], "r+b"))) return 2;
        if (argc > 3) budget = atol(argv[3]);
        struct hdf_io io = { 0, rd, wr, al, fr, lg };
        static struct xfat x;
        if (xfat_open(&x, &io) || ffs_init_exfat(&W, &x)) { printf("NOEXFAT\n"); return 1; }
        const char *why = xfat_writable(&x);
        if (why) { printf("FAIL not writable: %s\n", why); return 1; }
        wroot = x.root;
        write_test();
        ffs_sync(&W);
        fclose(img);
        printf("DONE %ld\n", nwrites);
        return fails != 0;
    }
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

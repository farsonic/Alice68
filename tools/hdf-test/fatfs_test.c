/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of amiga/hdf/fatfs.c (the folders' FAT engine) on a FAT image
 * made by mkfs.fat and filled by mtools (fatfs.sh). Every change it makes
 * on the image it also makes in a mirror directory on the PC; fatfs.sh then
 * compares the image (read by mtools) with the mirror and runs fsck.fat.
 *   fatfs_test IMAGE MIRROR_DIR */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fatfs.h"

static FILE *card;
static const char *mirror;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int rd(void *ctx, uint32_t s, uint32_t n, void *b) { (void)ctx; fseek(card, (long)s * 512, SEEK_SET); return fread(b, 512, n, card) == n ? 0 : -1; }
static int wr(void *ctx, uint32_t s, uint32_t n, const void *b) { (void)ctx; fseek(card, (long)s * 512, SEEK_SET); return fwrite(b, 512, n, card) == n ? 0 : -1; }
static void *al(void *ctx, uint32_t n) { (void)ctx; return calloc(1, n); }
static void fr(void *ctx, void *p, uint32_t n) { (void)ctx; (void)n; free(p); }
static void lg(void *ctx, const char *l) { (void)ctx; printf("  log: %s\n", l); }

static struct ffs V;

/* the content of a test file: depends on its seed and the offset */
static uint8_t pat(uint32_t seed, uint32_t off) { uint32_t x = seed * 2654435761u + off * 40503u; return (uint8_t)(x >> 13 ^ x >> 5 ^ off); }

/* Latin-1 path -> UTF-8 host path in the mirror */
static void host(const char *amiga, char *out)
{
    char *o = out + sprintf(out, "%s/", mirror);
    for (const unsigned char *p = (const unsigned char *)amiga; *p; p++) {
        if (*p < 0x80) *o++ = (char)*p;
        else { *o++ = (char)(0xc0 | *p >> 6); *o++ = (char)(0x80 | (*p & 0x3f)); }
    }
    *o = 0;
}

/* a path (from the partition root) -> its directory and entry */
static int lookup(const char *path, uint32_t *dir, struct ffs_ent *e)
{
    uint32_t d = 0;
    const char *p = path;
    for (;;) {
        uint32_t n = 0;
        while (p[n] && p[n] != '/') n++;
        int r = ffs_find(&V, d, p, n, e);
        if (r) return r;
        if (!p[n]) { if (dir) *dir = d; return 0; }
        if (!(e->attr & FFS_A_DIR)) return FFS_NOTDIR;
        d = e->first;
        p += n + 1;
    }
}
static uint32_t dir_of(const char *path, const char **leaf)
{
    const char *s = strrchr(path, '/');
    *leaf = s ? s + 1 : path;
    if (!s) return 0;
    char buf[512];
    memcpy(buf, path, (size_t)(s - path)); buf[s - path] = 0;
    struct ffs_ent e;
    if (lookup(buf, 0, &e) || !(e.attr & FFS_A_DIR)) return ~0u;
    return e.first;
}

static int t_mkdir(const char *path)
{
    const char *leaf;
    uint32_t d = dir_of(path, &leaf);
    struct ffs_ent e;
    int r = ffs_create(&V, d, leaf, (uint32_t)strlen(leaf), 1, &e);
    if (!r) { char h[1024]; host(path, h); mkdir(h, 0755); }
    return r;
}

/* write n bytes of pattern seed at pos, in pieces of chunk bytes (from an odd address if odd) */
static int t_write(const char *path, uint32_t pos, uint32_t n, uint32_t seed, uint32_t chunk, int odd, int create)
{
    const char *leaf;
    uint32_t d = dir_of(path, &leaf);
    struct ffs_ent e;
    int r = lookup(path, 0, &e);
    if (r == FFS_END && create) r = ffs_create(&V, d, leaf, (uint32_t)strlen(leaf), 0, &e);
    if (r) return r;
    struct ffs_file h;
    ffs_open(&h, &e);
    uint8_t *raw = malloc(chunk + 8), *buf = raw + (odd ? 1 : 0);
    char hp[1024]; host(path, hp);
    FILE *m = fopen(hp, create ? "a+b" : "r+b");
    if (!m) { m = fopen(hp, "w+b"); }
    for (uint32_t done = 0; done < n;) {
        uint32_t k = n - done < chunk ? n - done : chunk;
        for (uint32_t i = 0; i < k; i++) buf[i] = pat(seed, pos + done + i);
        int err;
        int32_t w = ffs_write(&V, &h, pos + done, buf, k, &err);
        if (w > 0) { fseek(m, pos + done, SEEK_SET); fwrite(buf, 1, (size_t)w, m); }
        if (err) { r = err; break; }
        done += k;
    }
    fclose(m);
    free(raw);
    int s = ffs_sync(&V);
    return r ? r : s;
}

static int t_truncate(const char *path, uint32_t size)
{
    struct ffs_ent e;
    int r = lookup(path, 0, &e);
    if (r) return r;
    struct ffs_file h;
    ffs_open(&h, &e);
    r = ffs_truncate(&V, &h, size);
    if (!r) { char hp[1024]; host(path, hp); r = truncate(hp, size) ? -100 : 0; }
    int s = ffs_sync(&V);
    return r ? r : s;
}

static int t_delete(const char *path)
{
    struct ffs_ent e;
    int r = lookup(path, 0, &e);
    if (r) return r;
    r = ffs_delete(&V, &e);
    if (!r) { char hp[1024]; host(path, hp); r = remove(hp) ? -100 : 0; }
    int s = ffs_sync(&V);
    return r ? r : s;
}

static int t_rename(const char *from, const char *to)
{
    struct ffs_ent e, o;
    int r = lookup(from, 0, &e);
    if (r) return r;
    const char *leaf;
    uint32_t nd = dir_of(to, &leaf);
    r = ffs_rename(&V, &e, nd, leaf, (uint32_t)strlen(leaf), &o);
    if (!r) { char a[1024], b[1024]; host(from, a); host(to, b); r = rename(a, b) ? -100 : 0; }
    int s = ffs_sync(&V);
    return r ? r : s;
}

/* read the whole file through the engine in pieces and compare with the mirror */
static void t_verify(const char *path, uint32_t chunk, int odd)
{
    struct ffs_ent e;
    int r = lookup(path, 0, &e);
    CHECK(!r, "%s: lookup %d", path, r);
    if (r) return;
    char hp[1024]; host(path, hp);
    FILE *m = fopen(hp, "rb");
    CHECK(m, "%s: not in the mirror", path);
    if (!m) return;
    fseek(m, 0, SEEK_END);
    long sz = ftell(m);
    fseek(m, 0, SEEK_SET);
    CHECK((long)e.size == sz, "%s: size %u, mirror %ld", path, e.size, sz);
    uint8_t *want = malloc((size_t)sz + 1), *raw = malloc(chunk + 8), *buf = raw + (odd ? 1 : 0);
    if (fread(want, 1, (size_t)sz, m) != (size_t)sz) CHECK(0, "mirror read");
    fclose(m);
    struct ffs_file h;
    ffs_open(&h, &e);
    uint32_t pos = 0;
    while (pos < (uint32_t)sz) {
        int32_t n = ffs_read(&V, &h, pos, buf, chunk);
        CHECK(n > 0, "%s: read at %u: %d", path, pos, n);
        if (n <= 0) break;
        if (memcmp(buf, want + pos, (size_t)n)) { CHECK(0, "%s: data differs in %u..%u", path, pos, pos + n); break; }
        pos += (uint32_t)n;
    }
    CHECK(ffs_read(&V, &h, pos, buf, chunk) == 0, "%s: read past the end", path);
    free(want); free(raw);
}

static int count_dir(uint32_t dir)
{
    struct ffs_ent e;
    uint32_t pos = 0;
    int n = 0;
    while (ffs_next(&V, dir, &pos, &e) == 0) n++;
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    card = fopen(argv[1], "r+b");
    mirror = argv[2];
    if (!card) return 3;
    struct hdf_io io = { 0, rd, wr, al, fr, lg };
    struct fat f;
    if (fat_open(&f, &io)) { printf("FAIL: fat_open\n"); return 1; }
    if (ffs_init(&V, &f)) return 1;
    ffs_amiga_to_fat(17841, 13 * 60 + 37, 50 * 42, &V.date, &V.time);   /* 2026-11-06 13:37:42 */
    uint32_t free0;
    CHECK(ffs_free_clusters(&V, &free0) == 0, "free count");
    printf("  FAT%d, %u clusters of %u bytes, %u free\n", f.fat32 ? 32 : 16, f.clusters, ffs_cluster_bytes(&V), free0);

    /* dates both ways */
    uint16_t dd, tt; uint32_t a, b, c;
    ffs_amiga_to_fat(17841, 13 * 60 + 37, 50 * 42, &dd, &tt);
    CHECK(dd == ((46 << 9) | (11 << 5) | 6) && tt == ((13 << 11) | (37 << 5) | 21), "date 2026-11-06: %04x %04x", dd, tt);
    ffs_fat_to_amiga(dd, tt, &a, &b, &c);
    CHECK(a == 17841 && b == 13 * 60 + 37 && c == 42 * 50, "date back: %u %u %u", a, b, c);
    ffs_amiga_to_fat(730 + 59, 0, 0, &dd, &tt);                       /* 1980-02-29 */
    CHECK(dd == ((0 << 9) | (2 << 5) | 29), "leap day %04x", dd);

    /* what mtools put there */
    uint32_t g;
    struct ffs_ent e;
    CHECK(ffs_resolve(&V, "Games", &g, &e) == 0 && (e.attr & FFS_A_DIR), "Games folder");
    CHECK(ffs_resolve(&V, "games/SUB", &g, &e) == 0, "games/SUB, any case");
    CHECK(ffs_resolve(&V, "Games/Long File Name.txt", &g, &e) == FFS_NOTDIR, "a file is not a folder");
    CHECK(ffs_resolve(&V, "", &g, &e) == FFS_BADNAME, "the root is not a folder to mount");
    t_verify("Games/Long File Name.txt", 1000, 0);
    t_verify("Games/readme.txt", 7, 1);
    t_verify("Games/SUB/BIG.BIN", 65536, 0);
    t_verify("Games/SUB/BIG.BIN", 4093, 1);
    CHECK(lookup("Games/README.TXT", 0, &e) == 0 && !strcmp(e.name, "readme.txt"), "8.3 lower-case flag: %s", e.name);
    CHECK(lookup("Games/LONGFI~1.TXT", 0, &e) == 0 && !strcmp(e.name, "Long File Name.txt"), "found by its 8.3 name");
    CHECK(lookup("Games/\xe4pfel \xdc.txt", 0, &e) == 0, "Latin-1 name from mtools");
    if (!lookup("Games/\xe4pfel \xdc.txt", 0, &e)) t_verify("Games/\xe4pfel \xdc.txt", 512, 0);
    CHECK(lookup("Games/\xc4PFEL \xfc.TXT", 0, &e) == 0, "Latin-1 without case");
    uint32_t many;
    CHECK(ffs_resolve(&V, "Games/Many", &many, &e) == 0 && count_dir(many) == 300, "300 entries in Many: %d", count_dir(many));
    for (int i = 0; i < 300; i += 37) { char p[64]; sprintf(p, "Games/Many/file number %03d.dat", i); t_verify(p, 300, 0); }

    /* new files of awkward sizes, in awkward pieces */
    static const uint32_t sizes[] = { 0, 1, 511, 512, 513, 4095, 4096, 4097, 65537, 1048576 + 3 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char p[64]; sprintf(p, "Games/new size %u.bin", sizes[i]);
        int r = t_write(p, 0, sizes[i], i + 100, i & 1 ? 1000 : 65536, i & 2, 1);
        CHECK(!r, "%s: %d", p, r);
        t_verify(p, 777, 1);
    }
    CHECK(t_write("Games/ALLCAPS.TXT", 0, 100, 7, 100, 0, 1) == 0, "8.3 name");
    CHECK(t_write("Games/lower.txt", 0, 100, 8, 100, 0, 1) == 0, "lower-case 8.3 name");
    CHECK(t_write("Games/Mixed.Txt", 0, 100, 9, 100, 0, 1) == 0, "mixed case: a long name");
    CHECK(t_write("Games/\xe9t\xe9 \xe0 la plage.doc", 0, 3000, 10, 3000, 0, 1) == 0, "Latin-1 name");
    CHECK(t_write("Games/a name that is quite a lot longer than thirteen characters, several pieces.txt", 0, 10, 11, 10, 0, 1) == 0, "long name");
    CHECK(t_write("Games/.hidden", 0, 10, 12, 10, 0, 1) == 0, "dot name");
    CHECK(t_write("Games/two.dots.in.name", 0, 10, 13, 10, 0, 1) == 0, "dots");
    struct ffs_ent x;
    CHECK(ffs_create(&V, g = dir_of("Games/x", &(const char *){0}), "lower.TXT", 9, 0, &x) == FFS_EXISTS, "exists, any case");
    CHECK(ffs_create(&V, g, "bad:name", 8, 0, &x) == FFS_BADNAME, "':' refused");
    CHECK(ffs_create(&V, g, "bad?", 4, 0, &x) == FFS_BADNAME, "'?' refused");
    CHECK(ffs_create(&V, g, "trailing.", 9, 0, &x) == FFS_BADNAME, "trailing dot refused");
    for (int i = 0; i < 12; i++) {                            /* ~1 .. ~12 */
        char p[96]; sprintf(p, "Games/Same beginning, number %d.txt", i);
        CHECK(t_write(p, 0, 50, 200 + i, 50, 0, 1) == 0, "%s", p);
    }

    /* changing files */
    CHECK(t_write("Games/new size 65537.bin", 1000, 5000, 77, 333, 1, 0) == 0, "overwrite inside");
    CHECK(t_write("Games/new size 65537.bin", 65537, 70000, 78, 4096, 0, 0) == 0, "append");
    t_verify("Games/new size 65537.bin", 4096, 0);
    CHECK(t_truncate("Games/new size 4097.bin", 100000) == 0, "extend with zeros");
    t_verify("Games/new size 4097.bin", 4096, 0);
    CHECK(t_truncate("Games/new size 1048579.bin", 5000) == 0, "shorten");
    t_verify("Games/new size 1048579.bin", 4096, 0);
    CHECK(t_truncate("Games/new size 513.bin", 0) == 0, "to nothing");
    CHECK(t_write("Games/new size 513.bin", 0, 9000, 79, 9000, 0, 0) == 0, "and again");
    t_verify("Games/new size 513.bin", 4096, 0);

    /* folders, renaming, moving */
    CHECK(t_mkdir("Games/Saves") == 0, "mkdir");
    CHECK(t_mkdir("Games/Saves/Deeper") == 0, "mkdir 2");
    CHECK(t_write("Games/Saves/Deeper/slot1.sav", 0, 2000, 80, 2000, 0, 1) == 0, "file in it");
    CHECK(t_rename("Games/lower.txt", "Games/Lower.txt") == 0, "case change");
    CHECK(t_rename("Games/Mixed.Txt", "Games/Saves/Moved here.txt") == 0, "move a file");
    CHECK(t_rename("Games/Saves/Deeper", "Games/SUB/Deeper moved") == 0, "move a folder");
    t_verify("Games/SUB/Deeper moved/slot1.sav", 512, 0);
    {
        uint32_t d, p;
        CHECK(ffs_resolve(&V, "Games/SUB/Deeper moved", &d, &e) == 0, "moved folder");
        CHECK(ffs_parent(&V, d, &p) == 0 && ffs_resolve(&V, "Games/SUB", &g, &e) == 0 && p == g, "its .. follows it");
    }
    CHECK(t_rename("Games/SUB", "Games/SUB/Deeper moved/inside") == FFS_LOOP, "a folder into itself");
    CHECK(t_rename("Games/ALLCAPS.TXT", "Games/Lower.txt") == FFS_EXISTS, "onto another file");
    CHECK(t_delete("Games/SUB") == FFS_NOTEMPTY, "delete a full folder");
    CHECK(t_mkdir("Games/Empty") == 0 && t_delete("Games/Empty") == 0, "delete an empty folder");

    /* a folder that grows past a cluster, then has holes, then fills them */
    CHECK(t_mkdir("Games/Lots") == 0, "Lots");
    for (int i = 0; i < 400; i++) {
        char p[128]; sprintf(p, "Games/Lots/a fairly long file name, number %04d.txt", i);
        int r = t_write(p, 0, (uint32_t)i, 300 + i, 64, 0, 1);
        if (r) { CHECK(0, "%s: %d", p, r); break; }
    }
    for (int i = 0; i < 400; i += 2) { char p[128]; sprintf(p, "Games/Lots/a fairly long file name, number %04d.txt", i); CHECK(t_delete(p) == 0, "%s", p); }
    for (int i = 0; i < 100; i++) { char p[128]; sprintf(p, "Games/Lots/x%03d", i); CHECK(t_write(p, 0, 10, 900 + i, 10, 0, 1) == 0, "%s", p); }
    uint32_t lots;
    CHECK(ffs_resolve(&V, "Games/Lots", &lots, &e) == 0 && count_dir(lots) == 300, "Lots has 300: %d", count_dir(lots));
    t_verify("Games/Lots/a fairly long file name, number 0399.txt", 64, 0);

    /* full card: write until there is no room, then give it back */
    uint32_t before;
    ffs_free_clusters(&V, &before);
    {
        struct ffs_ent fe;
        { char hp[1024]; host("Games/fill.bin", hp); fclose(fopen(hp, "wb")); }
        CHECK(ffs_create(&V, dir_of("Games/fill.bin", &(const char *){0}), "fill.bin", 8, 0, &fe) == 0, "fill.bin");
        struct ffs_file h;
        ffs_open(&h, &fe);
        static uint8_t blk[65536];
        int err = 0;
        uint32_t pos = 0;
        while (!err) { int32_t w = ffs_write(&V, &h, pos, blk, sizeof blk, &err); pos += (uint32_t)w; }
        CHECK(err == FFS_FULL, "full: %d", err);
        uint32_t now;
        ffs_free_clusters(&V, &now);
        CHECK(now == 0, "no free clusters left: %u", now);
        CHECK(h.e.size == pos, "size %u = written %u", h.e.size, pos);
        ffs_sync(&V);
        CHECK(t_delete("Games/fill.bin") == 0, "delete fill.bin");
        ffs_free_clusters(&V, &now);
        CHECK(now == before, "free again: %u, was %u", now, before);
    }
    CHECK(ffs_sync(&V) == 0, "sync");
    ffs_done(&V);
    fat_close(&f);
    fclose(card);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}

/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Host test of amiga/hdf/iso9660.c: walks an ISO image (made by genisoimage in
 * iso.sh) as alice68hdf.device would, through a struct hdf_file of 512-byte
 * blocks, and prints every entry ("D path" / "F path size crc32") for iso.sh to
 * compare with the source tree. Checks on the way: each name found again by
 * iso_find in another case, each entry's parent, reads in odd pieces into an
 * odd buffer address equal to one whole read.
 *     iso_test IMAGE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "iso9660.h"

static FILE *img;
static int fails;
static int rd(void *ctx, uint32_t sec, uint32_t n, void *buf)
{
    (void)ctx;
    if (fseek(img, (long)sec * 512, SEEK_SET)) return -1;
    return fread(buf, 512, n, img) == n ? 0 : -1;
}
static void *al(void *ctx, uint32_t n) { (void)ctx; return calloc(1, n); }
static void fr(void *ctx, void *p, uint32_t n) { (void)ctx; (void)n; free(p); }
static void lg(void *ctx, const char *l) { (void)ctx; fprintf(stderr, "log: %s\n", l); }

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}

#define FAIL(...) do { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } while (0)

static void walk(struct iso *v, const struct iso_ent *dir, const char *path, int depth)
{
    uint32_t pos = 0;
    struct iso_ent e;
    int r;
    if (depth > 40) { FAIL("%s: too deep", path); return; }
    while ((r = iso_next(v, dir, &pos, &e)) == ISO_OK) {
        char p[4096];
        snprintf(p, sizeof p, "%s%s%s", path, path[0] ? "/" : "", e.name);
        /* found again, in upper case */
        char up[ISO_NAME];
        uint32_t n = (uint32_t)strlen(e.name);
        for (uint32_t i = 0; i <= n; i++) up[i] = (e.name[i] >= 'a' && e.name[i] <= 'z') ? e.name[i] - 32 : e.name[i];
        struct iso_ent f;
        if (iso_find(v, dir, up, n, &f) || f.extent != e.extent || f.isdir != e.isdir) FAIL("%s: iso_find", p);
        /* its parent is this directory */
        struct iso_ent par;
        if (iso_parent(v, &e, &par) || par.extent != dir->extent || !par.isdir) FAIL("%s: parent %u, want %u", p, par.extent, dir->extent);
        if (e.isdir) {
            printf("D %s\n", p);
            walk(v, &e, p, depth + 1);
        } else {
            uint8_t *whole = malloc(e.size + 1), *pieces = malloc(e.size + 8);
            int32_t got = iso_read(v, &e, 0, whole, e.size + 1);   /* asks for more: gets the size */
            if (got != (int32_t)e.size) FAIL("%s: read %d of %u", p, got, e.size);
            uint32_t at = 0;
            while (at < e.size) {                               /* 777-byte pieces into an odd address */
                int32_t k = iso_read(v, &e, at, pieces + 1 + at, 777);
                if (k <= 0) { FAIL("%s: piece at %u: %d", p, at, k); break; }
                at += (uint32_t)k;
            }
            if (memcmp(whole, pieces + 1, e.size)) FAIL("%s: pieces differ", p);
            if (iso_read(v, &e, e.size, whole, 10) != 0) FAIL("%s: read at the end", p);
            printf("F %s %u %08x\n", p, e.size, crc32(whole, e.size));
            free(whole); free(pieces);
        }
    }
    if (r != ISO_END) FAIL("%s: iso_next %d", path, r);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !(img = fopen(argv[1], "rb"))) return 2;
    fseek(img, 0, SEEK_END);
    long size = ftell(img);
    struct hdf_io io = { 0, rd, 0, al, fr, lg };
    struct hdf_extent ext = { 0, 0, (uint32_t)(size / 512) };
    struct hdf_file f;
    memset(&f, 0, sizeof f);
    strcpy(f.name, "test.iso");
    f.size = (uint32_t)size; f.blocks = (uint32_t)(size / 512); f.next = f.cap = 1; f.ext = &ext;
    struct iso v;
    int r = iso_open(&v, &io, &f);
    if (r) { printf("FAIL iso_open %d\n", r); return 1; }
    printf("V %s %s\n", v.rr ? "rockridge" : v.joliet ? "joliet" : "iso9660", v.vol);
    struct iso_ent par;
    if (iso_parent(&v, &v.root, &par) || par.extent != v.root.extent) FAIL("the root's parent");
    uint32_t d, m, t;
    iso_date(v.root.date, &d, &m, &t);
    printf("T %u\n", d);
    walk(&v, &v.root, "", 0);
    iso_close(&v);
    /* not an ISO: the first 64 KB zeroed */
    printf(fails ? "iso_test FAILED (%d)\n" : "iso_test OK\n", fails);
    return fails != 0;
}

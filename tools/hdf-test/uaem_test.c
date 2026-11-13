/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of amiga/hdf/uaem.c: FS-UAE-style sidecars and %xx names. */
#include <stdio.h>
#include <string.h>
#include "uaem.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void name(const char *amiga, const char *want)
{
    char host[128], back[128];
    int n = uaem_encode(amiga, (uint32_t)strlen(amiga), host, sizeof host);
    CHECK(n >= 0 && !strcmp(host, want), "encode \"%s\": \"%s\", want \"%s\"", amiga, n >= 0 ? host : "(too long)", want);
    uaem_decode(host, back, sizeof back);
    CHECK(!strcmp(back, amiga), "decode \"%s\": \"%s\"", host, back);
}

int main(void)
{
    name("plain.txt", "plain.txt");
    name("What?.txt", "What%3f.txt");
    name("100%", "100%25");
    name("a*b\"c<d>e|f\\g", "a%2ab%22c%3cd%3ee%7cf%5cg");
    name("ends with a dot.", "ends with a dot%2e");
    name("ends with space ", "ends with space%20");
    name("CON", "CO%4e");
    name("aux.info", "au%78.info");
    name("COM1.txt", "CO%4d1.txt");
    name("COMMAND", "COMMAND");
    name("my.uaem", "my.uae%6d");
    name("\xe4pfel", "\xe4pfel");                           /* Latin-1 stays as it is */
    char h[16];
    CHECK(uaem_encode("**********", 10, h, sizeof h) == -1, "a name too long once escaped");
    char d[64];
    uaem_decode("bad%zz and %4", d, sizeof d);
    CHECK(!strcmp(d, "bad%zz and %4"), "a %% that is not an escape stays: %s", d);

    CHECK(uaem_hidden("x.uaem") && uaem_hidden("X.UAEM") && uaem_hidden("._x") && uaem_hidden(".DS_Store") &&
          uaem_hidden("_UAEFSDB.___") && !uaem_hidden("uaem") && !uaem_hidden(".profile") && !uaem_hidden("my.uae%6d"), "hidden names");
    CHECK(uaem_sidecar("Read Me", d, sizeof d) == 12 && !strcmp(d, "Read Me.uaem"), "sidecar name");

    /* FS-UAE's own example, and the round trip */
    struct uaem m;
    const char *ex = "----rwed 2013-02-12 21:20:52.02 hello\n";
    CHECK(uaem_parse((const uint8_t *)ex, (uint32_t)strlen(ex), &m) == 0, "parse the FS-UAE example");
    CHECK(m.prot == 0 && !strcmp(m.comment, "hello") && m.mins == 21 * 60 + 20 && m.ticks == 52 * 50 + 1, "fields %x %u %u %s", m.prot, m.mins, m.ticks, m.comment);
    CHECK(m.days == 12826, "2013-02-12 is day %u, want 12826", m.days);
    char line[UAEM_LINE];
    uint32_t n = uaem_format(&m, line);
    CHECK(n == strlen(ex) && !memcmp(line, ex, n), "format: %.*s", (int)n, line);
    const char *s2 = "\xef\xbb\xbfhs--r-e- 1999-12-31 23:59:59.98 a script, with a comment\r\n";
    CHECK(uaem_parse((const uint8_t *)s2, (uint32_t)strlen(s2), &m) == 0 && m.prot == (0x80 | 0x40 | 0x04 | 0x01) &&
          !strcmp(m.comment, "a script, with a comment") && m.ticks == 59 * 50 + 49, "BOM, flags hs, r-e-, CR LF: %x %s", m.prot, m.comment);
    n = uaem_format(&m, line);
    CHECK(!memcmp(line, "hs--r-e- 1999-12-31 23:59:59.98 a script, with a comment\n", n), "format 2: %.*s", (int)n, line);
    const char *s3 = "----rwed 2013-02-12 21:20:52.02\n";
    CHECK(uaem_parse((const uint8_t *)s3, (uint32_t)strlen(s3), &m) == 0 && !m.comment[0], "no comment");
    const char *bad[] = { "rwed 2013-02-12 21:20:52.02\n", "----rwex 2013-02-12 21:20:52.02\n", "----rwed 2013-02-30 21:20:52.02\n",
                          "----rwed 2013-02-12 25:20:52.02\n", "----rwed 1977-02-12 21:20:52.02\n", "" };
    for (int i = 0; i < 6; i++) CHECK(uaem_parse((const uint8_t *)bad[i], (uint32_t)strlen(bad[i]), &m) == -1, "bad line %d taken", i);
    CHECK(uaem_plain(0) && uaem_plain(0x10) && uaem_plain(0x05) && uaem_plain(0x15) && !uaem_plain(0x40) && !uaem_plain(0x01) && !uaem_plain(0x02), "plain");
    printf(fails ? "uaem_test FAILED (%d)\n" : "uaem_test ALL OK\n", fails);
    return fails != 0;
}

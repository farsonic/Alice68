/* SPDX-License-Identifier: GPL-2.0-only
 * Host test of the setup screen's round trip: alice_setup.c (the kernel's screen) turns
 * keys into an edit list, amiga/hdf/cmdedit.c applies it to cmdline.txt, and the new
 * file read back gives the screen the values that were chosen. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "cmdedit.h"
#include "alice_setup.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* HID usages */
enum { ENTER = 0x28, ESC = 0x29, BKSP = 0x2a, TAB = 0x2b, F10 = 0x43, DEL = 0x4c, RIGHT = 0x4f, LEFT = 0x50, DOWN = 0x51, UP = 0x52 };

static struct as_state st;
static uint16_t fb[800 * 600];

static int keys(const uint8_t *k, unsigned n, uint8_t mods)
{
    int r = AS_NONE;
    for (unsigned i = 0; i < n; i++) { r = as_key(&st, k[i], mods); if (r == AS_REDRAW) as_draw(&st); }
    return r;
}
static void type(const char *s)
{
    for (; *s; s++) {
        uint8_t u = 0, m = 0;
        char c = *s;
        if (c >= 'a' && c <= 'z') u = (uint8_t)(0x04 + c - 'a');
        else if (c >= 'A' && c <= 'Z') { u = (uint8_t)(0x04 + c - 'A'); m = 0x02; }
        else if (c >= '1' && c <= '9') u = (uint8_t)(0x1e + c - '1');
        else if (c == '0') u = 0x27;
        else if (c == ' ') u = 0x2c;
        else if (c == '-') u = 0x2d;
        else if (c == '_') { u = 0x2d; m = 0x02; }
        else if (c == '.') u = 0x37;
        as_key(&st, u, m);
    }
}

int main(void)
{
    al_setup(fb, 800 * 2, 800, 600);                 /* drawing works on a small screen too */

    /* cmd_apply on its own */
    char out[1024];
    int n = cmd_apply("aga.look=0 aga.scale=1 eth=off hdf.boot=Old wifi.ssid=\"A B\"\n",
                      "-aga.scale\n+aga.scale=4\n-eth\n-wifi.ssid\n+wifi.ssid=\"My Net\"\n", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "aga.look=0 hdf.boot=Old aga.scale=4 wifi.ssid=\"My Net\"\n"), "apply: %s", out);
    n = cmd_apply("hdf=off hdf.readonly hdf.ro hdfx=1 iso=off\r\n", "-hdf\n-hdf.readonly\n-hdf.ro\n", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "hdfx=1 iso=off\n"), "drop by name only: %s", out);
    n = cmd_apply("", "+eth=on\n", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "eth=on\n"), "empty file: %s", out);
    n = cmd_apply("aga.look=0", "+x=1\n", out, 12);
    CHECK(n == -1, "too long refused (%d)", n);
    n = cmd_apply("a b\nc", "", out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "a b c\n"), "two lines become one: %s", out);

    /* the screen: values from bootargs (firmware options in front are left alone) */
    const char *boot = "coherent_pool=1M 8250.nr_uarts=1 aga.look=2 aga.scale=1 eth=off hdf.boot=Prism_Desktop.hdf "
                       "wifi.ssid=\"Home Net\" net.trace=1";
    struct al_info info[] = { { "Version", "0.5.0" } };
    as_init(&st, boot, info, 1);
    as_draw(&st);
    CHECK(!as_changed(&st), "nothing changed at first");
    CHECK(as_edits(&st, out, sizeof out) == 0, "no edits at first: %s", out);
    uint8_t esc[] = { ESC };
    CHECK(keys(esc, 1, 0) == AS_EXIT, "Esc with nothing changed leaves");

    as_init(&st, boot, info, 1);
    /* Display: Picture TV -> Sharp (right wraps), Scaling Integer -> 16:9 */
    uint8_t k1[] = { RIGHT, DOWN, RIGHT, RIGHT, RIGHT };
    keys(k1, sizeof k1, 0);
    /* Storage: Hardfiles -> read-only, Boot hardfile -> typed, exFAT -> read-write */
    uint8_t k2[] = { TAB, RIGHT, DOWN, ENTER };
    keys(k2, sizeof k2, 0);
    for (int i = 0; i < 20; i++) as_key(&st, BKSP, 0);
    type("Work bench");
    uint8_t k3[] = { ENTER, DOWN, DOWN, RIGHT, RIGHT, RIGHT };
    keys(k3, sizeof k3, 0);
    /* Network: SSID cleared with Del, password typed, Ethernet off -> "when opened" (default) */
    uint8_t k4[] = { TAB, DOWN, DEL, DOWN, ENTER };
    keys(k4, sizeof k4, 0);
    type("pass word1");
    uint8_t k5[] = { ENTER, DOWN, DOWN, DEL };
    keys(k5, sizeof k5, 0);
    CHECK(as_changed(&st), "changed");
    char e[768];
    n = as_edits(&st, e, sizeof e);
    CHECK(n > 0, "edits (%d)", n);
    const char *want = "-aga.look\n+aga.look=0\n-aga.scale\n+aga.scale=4\n-hdf\n-hdf.readonly\n-hdf.ro\n+hdf.readonly\n"
                       "-hdf.boot\n+hdf.boot=Work_bench\n-alice68.exfat\n+alice68.exfat=rw\n-wifi.ssid\n"
                       "-wifi.psk\n+wifi.psk=\"pass word1\"\n-eth\n";
    CHECK(!strcmp(e, want), "edit list:\n%s---- want:\n%s", e, want);
    /* F10 asks, N goes back, F10 and Y saves */
    uint8_t f10[] = { F10 }, nk[] = { 0x11 }, yk[] = { 0x1c };
    CHECK(keys(f10, 1, 0) == AS_REDRAW && st.dialog == 1, "F10 asks");
    CHECK(keys(nk, 1, 0) == AS_REDRAW && !st.dialog, "N goes back");
    keys(f10, 1, 0);
    CHECK(keys(yk, 1, 0) == AS_SAVE, "Y saves");

    /* alice68hdf's side, and the result read back by the screen */
    n = cmd_apply("aga.look=2 aga.scale=1 eth=off hdf.boot=Prism_Desktop.hdf wifi.ssid=\"Home Net\" net.trace=1\n", e, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "net.trace=1 aga.look=0 aga.scale=4 hdf.readonly hdf.boot=Work_bench alice68.exfat=rw wifi.psk=\"pass word1\"\n"),
          "new cmdline.txt: %s", out);
    static struct as_state again;
    as_init(&again, out, info, 1);
    CHECK(!memcmp(again.v.cur, st.v.cur, sizeof st.v.cur), "choices read back");
    CHECK(!strcmp(again.v.text[4], "Work bench") && !again.v.text[8][0] && !strcmp(again.v.text[9], "pass word1"), "texts read back: '%s' '%s' '%s'",
          again.v.text[4], again.v.text[8], again.v.text[9]);

    /* Esc with changes asks first */
    as_init(&st, boot, info, 1);
    uint8_t k6[] = { RIGHT, ESC };
    CHECK(keys(k6, 2, 0) == AS_REDRAW && st.dialog == 2, "Esc with changes asks");
    CHECK(keys(yk, 1, 0) == AS_EXIT, "Y leaves without saving");
    /* a quote can't be typed into a quoted option; the country is upper case, two letters */
    as_init(&st, "", info, 1);
    uint8_t k7[] = { TAB, TAB, DOWN, DOWN, DOWN, ENTER };
    keys(k7, sizeof k7, 0);
    type("gbx");
    as_key(&st, ENTER, 0);
    CHECK(!strcmp(st.v.text[10], "GB"), "country: %s", st.v.text[10]);
    as_init(&st, "", info, 1);
    uint8_t k8[] = { TAB, TAB, DOWN, ENTER };
    keys(k8, sizeof k8, 0);
    as_key(&st, 0x34, 0x02);                         /* Shift + ' : a double quote */
    CHECK(!strchr(st.edit, '"'), "no quote in an SSID: %s", st.edit);

    printf(fails ? "cmdedit_test FAILED (%d)\n" : "cmdedit_test ALL OK\n", fails);
    return fails != 0;
}

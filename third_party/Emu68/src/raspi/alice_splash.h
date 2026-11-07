/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 boot splash: header, status strip, SYSTEM and USB log panels,
 * credits line. Self-contained (framebuffer, Topaz glyphs, the generated
 * header picture), so tools/splash-preview can build it on a PC and render
 * the exact screen the Pi shows.
 */
#ifndef ALICE_SPLASH_H
#define ALICE_SPLASH_H
#include <stdint.h>

#define ALICE_NAME    "Alice68"
#define ALICE_VERSION "0.4.0"

/* stage bits as frf_boot_stage() numbers them */
struct al_status {
    uint32_t stages;            /* bit n: stage n reached */
    int keyboards, mice;        /* -1: not known yet */
    int usb_fail;               /* the USB-A host gave up */
    int hdf_boot, hdf_other, hdf_left;  /* hardfiles: bootable, mounted not bootable, left out; -1 not known yet */
    const char *wifi;           /* the WiFi's state in a few words; 0 = no WiFi chip/driver (yet) */
    int wifi_col;               /* 0 waiting, 1 good, 2 failed */
    const char *eth;            /* the kernel's Ethernet, as the WiFi; 0 or "" = not in use */
    int eth_col;
};

void al_setup(uint16_t *fb, uint32_t pitch_bytes, uint32_t w, uint32_t h);
void al_clear(void);
void al_header(const char *l1, const char *l2, const char *l3);
void al_status_draw(const struct al_status *s);
void al_footer(void);
/* which: 0 SYSTEM (left), 1 USB (right). lines[0..n-1], oldest first; the
   panel shows the newest that fit. Returns 0 if the panel has no room. */
int  al_panel(int which, const char *const *lines, unsigned n);
unsigned al_panel_cols(int which);
/* the header's animation, ms after it began (the kernel calls it about 20 times a second
   while the splash is up); nothing when the header has no room for it */
void al_anim(uint32_t ms, const struct al_status *s);
#endif

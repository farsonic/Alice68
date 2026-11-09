/* SPDX-License-Identifier: GPL-2.0-only
 * Renders the Alice68 boot splash on a PC with the kernel's own drawing code
 * (src/raspi/alice_splash.c), to a PPM: build.sh does it all.
 * build.sh W H MS: with the header's animation as it is MS ms after it began. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "alice_splash.h"

static const char *sys_log[] = {
    "[BOOT] Alice68 0.3.0 - Emu68 1.1.0-beta.1 (05.11.2026)",
    "[BOOT] Boot address is 00080000",
    "[BOOT] Build ID: b4dcb52",
    "[BOOT] ARM stack top at 0xffffff9000200000",
    "[BOOT] Display size is 1920x1080",
    "[BOOT] Framebuffer @ 3e8fa000",
    "[BOOT] USB DMA pool: 0x38200000-0x383fffff (2 MB: 1 MB uncached, 1 MB cached bounce)",
    "[BOOT] Kickstart: kick31.rom, 524288 bytes, 3.1 (40.068)",
    "[STAGE 1] splash",
    "[AGA] chip RAM 2048 KB, virtual chipset on CPU2",
    "[AGA] HDMI audio at 48000 Hz",
    "[STAGE 2] USB released",
    "[STAGE 3] USB gate done",
    "[FRF-XHCI] decision: BCM2711 yes, DMA pool 0x38200000, pcie0 found -> kernel drives the USB-A ports",
    "[BOOT] Z3 RAM board: 256 MB at 0x40000000",
    "[STAGE 4] guest input armed",
    "[STAGE 5] 68k jump",
    "[STAGE 6] CPU1 released",
    "[AGA] first frame: 640x256 lores -> 1920x1080, scale 4",
    "[alice68hdf] alice68hdf.device 0.3 (05.11.2026) Alice68 hardfiles",
    "[alice68hdf] Workbench.hdf: 512 MB, 1 piece on the card",
    "[alice68hdf] cmdline.txt: hdf.boot=Workbench",
    "[alice68hdf] unit 0: Workbench.hdf, 1 partition, hdf.boot",
    "[alice68hdf]   DH0: replaces the drive mounted as DH0 (hdf.boot)",
    "[alice68hdf]   DH0: 511 MB, DOS\\3, boot priority 1",
    "[alice68hdf]   filesystem PFS\\3 19.2 loaded from the RDB",
    "[alice68hdf]   DH5: volume Work is mounted already (on DH1) - not mounted",
    "[alice68hdf] 3 units ready",
    "[alice68hdf] hardfiles: 1 bootable, 2 non-bootable, 0 left out",
};
static const char *usb_log[] = {
    "PCIe link up, Gen 2 x1",
    "VL805 1106:3483, firmware reload by VideoCore OK",
    "xHCI 1.0, 32 slots, 5 ports - controller running",
    "port 1: 2109:3431, high speed, class 09, slot 1",
    "port 1: hub, 4 ports",
    "port 1.2: 05ac:026c, full speed, class 00, slot 2",
    "port 1.2: keyboard ready (endpoint 1, 8 bytes) -> Amiga keyboard",
    "port 1.3: 30fa:0300, low speed, class 00, slot 3",
    "port 1.3: mouse ready (endpoint 1, 4 bytes) -> Amiga mouse port",
    "port 2: 0bda:8151, high speed, class 00, slot 4",
    "port 2: Realtek RTL815x network adapter in driver-CD mode - virtual eject (mode switch)",
    "port 2: mode switch sent (status 0) - waiting for it to reappear as a network adapter",
    "port 2: 0bda:8156, super speed, class 00, slot 5",
    "port 2: 0bda:8156 -> Neptune, alice68-usb.device port 1",
    "boot scan done: 1 keyboard(s), 1 mouse/mice",
};

int main(int argc, char **argv)
{
    uint32_t w = argc > 1 ? (uint32_t)atoi(argv[1]) : 1920, h = argc > 2 ? (uint32_t)atoi(argv[2]) : 1080;
    uint16_t *fb = calloc((size_t)w * h, 2);
    al_setup(fb, w * 2, w, h);
    al_clear();
    char l2[96], l3[64];
    snprintf(l2, sizeof l2, "based on Emu68 1.1.0-beta.1 - AGA core 1.120");
    snprintf(l3, sizeof l3, "Raspberry Pi 4 - display %ux%u", w, h);
    al_header(ALICE_NAME " " ALICE_VERSION, l2, l3);
    struct al_status s = { 0x7fe, 1, 1, 0, 1, 2, 0, "connected MyNetwork", 1, "1000 Mbit/s", 1 };
    al_status_draw(&s);
    al_panel(0, sys_log, sizeof sys_log / sizeof *sys_log);
    al_panel(1, usb_log, sizeof usb_log / sizeof *usb_log);
    al_footer();
    if (argc > 3) al_anim((uint32_t)atoi(argv[3]), &s);    /* the header's animation at that ms */
    printf("P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 31) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(rgb, 1, 3, stdout);
    }
    return 0;
}

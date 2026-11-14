/* SPDX-License-Identifier: GPL-2.0-only
 * Renders the Alice68 boot splash on a PC with the kernel's own drawing code
 * (src/raspi/alice_splash.c), to a PPM: build.sh does it all.
 * build.sh W H [splash|prompt]: the boot splash, with the setup key in the footer for prompt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "alice_splash.h"
#include "alice_setup.h"

/* the setup screen after some keys: setup, setup-storage, setup-edit, setup-save, setup-system */
static void preview_setup(const char *mode)
{
    static struct as_state st;
    struct al_info info[] = { { "Version", "0.5.0" }, { "Build", "6be46ab (14.11.2026)" }, { "Emu68", "1.1.0-beta.1" },
                              { "AGA core", "1.120" }, { "Display", "1920 x 1080" }, { "Keyboards", "1" }, { "Mice", "1" } };
    as_init(&st, "coherent_pool=1M aga.look=0 aga.scale=1 eth=off hdf.boot=PrismDesktop-0.2.23-Alice68-Workbench.hdf "
                 "wifi.ssid=\"My Network\" wifi.psk=secret12", info, sizeof info / sizeof *info);
    static const uint8_t storage[] = { 0x2b, 0x51 }, edit[] = { 0x2b, 0x51, 0x28, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x05, 0x13 },
                         save[] = { 0x51, 0x4f, 0x4f, 0x2b, 0x51, 0x51, 0x51, 0x4f, 0x51, 0x43 }, system_[] = { 0x2b, 0x2b, 0x2b, 0x2b },
                         display[] = { 0x51, 0x4f, 0x4f, 0x4f };
    const uint8_t *k = 0; unsigned n = 0;
    if (!strcmp(mode, "setup-storage")) { k = storage; n = sizeof storage; }
    else if (!strcmp(mode, "setup-edit")) { k = edit; n = sizeof edit; }
    else if (!strcmp(mode, "setup-save")) { k = save; n = sizeof save; }
    else if (!strcmp(mode, "setup-system")) { k = system_; n = sizeof system_; }
    else { k = display; n = sizeof display; }
    for (unsigned i = 0; i < n; i++) as_key(&st, k[i], 0);
    as_draw(&st);
    char e[512];
    if (as_edits(&st, e, sizeof e) > 0) fprintf(stderr, "edits:\n%s", e);
}

static const char *sys_log[] = {
    "[BOOT] Alice68 0.5.0 - Emu68 1.1.0-beta.1 - AGA core 1.120",
    "[BOOT] Display size is 1920x1080",
    "[BOOT] USB DMA pool: 0x38200000-0x383fffff (2 MB: 1 MB uncached, 1 MB cached bounce)",
    "[BOOT] Kickstart: kick31.rom, 524288 bytes, 3.1 (40.068)",
    "[STAGE 1] splash",
    "[AGA] chip RAM 2048 KB, virtual chipset on CPU3",
    "[STAGE 2] USB released",
    "[STAGE 3] USB gate done",
    "[BOOT] Z3 RAM board: 256 MB at 0x40000000",
    "[STAGE 5] 68k jump",
    "[alice68hdf] alice68hdf.device 0.7 (14.11.2026) Alice68 hardfiles, folders and CD images",
    "[alice68hdf] card: brcm-emmc.device unit 0, NSD 64-bit",
    "[alice68hdf] FAT32 partition at sector 8192, 1046528 clusters of 32768 bytes",
    "[WIFI] mailbox tag 38041: error -4, again",
    "[alice68hdf] cmdline.txt: hdf.boot=PrismDesktop-0.2.23-Alice68-Workbench.hdf",
    "[alice68hdf] unit 0: PrismDesktop-0.2.23-Alice68-Workbench.hdf, single partition, hdf.boot",
    "[alice68hdf]   DH0: replaces the drive mounted as DH0 (hdf.boot)",
    "[alice68hdf]   DH0: 64 MB, DOS\\1, boot priority 0",
    "[alice68hdf] unit 1: system_rdb.hdf, 1 partition",
    "[alice68hdf]   DH0: a drive of that name is mounted already - mounted as HF10",
    "[alice68hdf]   HF10: 1023 MB, DOS\\3, not bootable",
    "[alice68hdf] unit 2: PrismDesktop-0.1.7-Workbench.hdf, single partition",
    "[alice68hdf]   DH1: volume Workbench is mounted already (on DH0) - not mounted",
    "[WIFI] SDIO: 4-bit bus, 25000000 Hz",
    "[WIFI] chip 4345 rev 6: cyfmac43455-sdio-standard.bin, brcmfmac43455-sdio.txt",
    "[WIFI] firmware running (67 ms)",
    "[alice68hdf] 8 units ready",
    "[alice68hdf] hardfiles: 1 bootable, 1 non-bootable, 6 left out",
    "[WIFI] ready - no network yet (Alice68-WiFi JOIN, or wifi.ssid= in cmdline.txt)",
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
    const char *mode = argc > 3 ? argv[3] : "splash";
    uint16_t *fb = calloc((size_t)w * h, 2);
    al_setup(fb, w * 2, w, h);
    al_clear();
    char disp[32];
    snprintf(disp, sizeof disp, "%u x %u", w, h);
    struct al_info rows[] = { { "Version", ALICE_VERSION }, { "Build", "6be46ab (14.11.2026)" },
                              { "Emu68", "1.1.0-beta.1" }, { "AGA core", "1.120" }, { "Display", disp } };
    al_header(rows, sizeof rows / sizeof *rows);
    struct al_status s = { 0x7fe, 1, 1, 0, 1, 1, 6, "connected MyNetwork", 1, 0, 0 };
    al_status_draw(&s);
    al_panel(0, sys_log, sizeof sys_log / sizeof *sys_log);
    al_panel(1, usb_log, sizeof usb_log / sizeof *usb_log);
    al_footer(!strcmp(mode, "prompt") ? "DEL" : 0, "Setup");
    if (!strncmp(mode, "setup", 5)) preview_setup(mode);
    printf("P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 31) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(rgb, 1, 3, stdout);
    }
    free(fb);
    return 0;
}

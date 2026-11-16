/* SPDX-License-Identifier: GPL-2.0-only
 *
 * cardcore: the SD card Alice68 booted from, without a window. Used by Alice68Prefs's Card,
 * Files, Settings, Text files and Bluetooth pages and by the Shell's `Alice68 CARD` and
 * `Alice68 BT`, so both do the same and amiga/test/harness_cardcmd.py tests it.
 *
 * Everything goes through alice68hdf.device's card service (hdf/cardsvc.h), which writes with
 * the device's own FAT and exFAT engines, so mounted hardfiles, CD images and open files are
 * safe. cmdline.txt and config.txt are read by cc_load() and written only by cc_save(), the old
 * ones kept as cmdline.txt.bak and config.txt.bak. The one exception is Bluetooth: a pairing is
 * kept in cmdline.txt (bt.dev=) at once by cc_bt_keep(), or it would be lost at the next start.
 *
 * The WiFi password (wifi.psk=) is never shown: cc_cmdline_shown() has it as CC_PMASK, and a
 * line typed with CC_PMASK in its place keeps the password as it is (cc_cmdline_typed()).
 *
 * Plain C types only: Alice68Prefs (built with Riptide's headers) includes it too.
 */
#ifndef ALICE68_CARDCORE_H
#define ALICE68_CARDCORE_H
#include "hdf/cardsvc.h"
#include "../third_party/Emu68/src/raspi/alice_opts.h"

#define CC_PMASK   "********"
#define CC_MAXF    256                      /* entries of a folder listed */
#define CC_MAXL    160                      /* config.txt lines */
#define CC_LINE    200
#define CC_MAXROM  32

struct cc_ent { char name[108]; unsigned int size, sizehi, flags; };

/* 0, or a CSE_* (CSE_NOVOL: no card service: alice68hdf.device too old, or not Alice68); -1 no memory */
int  cc_open(void);
void cc_close(void);
const char *cc_err(int e);                  /* a CSE_* as words */
int  cc_ends(const char *s, const char *x); /* s ends in x, any case */

/* the card: partitions, free space (count_free: also on FAT, slower), what is mounted from it */
int  cc_info(struct cs_info *out, int count_free);
/* a folder: folders first, then by name; *more: it has more than max entries */
int  cc_list(unsigned vol, const char *path, struct cc_ent *e, int max, int *n, int *more);
int  cc_delete(unsigned vol, const char *path);
int  cc_rename(unsigned vol, const char *from, const char *to);
int  cc_mkdir(unsigned vol, const char *path);
int  cc_is_rom(const char *name, unsigned int size);
void cc_child(char *out, const char *dir, const char *name);   /* dir/name, CS_PATH */
void cc_parent(char *path);                                    /* path one up */

/* cmdline.txt and config.txt: cc_load() reads both (0, or a CSE_*) */
int  cc_load(void);
struct opt_vals *cc_opts(void);             /* the Settings page's values; change them, then cc_* below */
const char *cc_cmdline(void);               /* what cc_save() writes (with the password) */
void cc_cmdline_shown(char *out, unsigned max);   /* the same, wifi.psk= as CC_PMASK, one line */
void cc_cmdline_typed(const char *line);    /* a line typed in: the options follow it */
int  cc_secret_item(void);                  /* the password's item in opt_items, -1 none */
void cc_set_text(int i, const char *v);    /* text item i as typed (upper case, no '"', its length) */
int  cc_set_option(const char *word);       /* KEY=VALUE, an option as written, or LABEL=NAME; 0 or -1 */
void cc_option_shown(int i, char *out, unsigned max);   /* item i's value as shown (password masked) */
void cc_set_boot(const char *hdfname);      /* hdf.boot= the hardfile's name without .hdf */

int  cc_nlines(void);
const char *cc_line(int i);
int  cc_line_set(int i, const char *s);     /* i == cc_nlines(): a new last line; 0 or -1 */
int  cc_line_insert(int i, const char *s);
int  cc_line_delete(int i);
int  cc_nroms(void);
const char *cc_rom(int i);                  /* the ROMs in the FAT root and the one config.txt names */
int  cc_rom_cur(void);                      /* config.txt's (initramfs NAME followkernel), -1 none */
int  cc_set_kickstart(const char *name);    /* that line made or changed; 0 or -1 */

int  cc_changed(void);                      /* 1 cmdline.txt, 2 config.txt differ from the card */
/* writes what changed (.bak first): 1 | 2 what was written, 0 nothing to write, or -CSE_* */
int  cc_save(int *failed_file);             /* *failed_file: 1 cmdline.txt, 2 config.txt */

/* the network board (alice_net.h): 0 when this is not an Alice68 kernel with it */
volatile unsigned char *cc_board(void);
int  cc_restart(void);                      /* asks the kernel to restart the Pi; -1 no board */

/* Bluetooth, through the board's struct an_bt */
int  cc_bt_cmd(unsigned cmd, const volatile unsigned char *addr);   /* 0, -1 no board, -2 not ready */
int  cc_bt_keep(void);                      /* bt.dev= in cmdline.txt from the paired list: 0 or a CSE_* (-1 too long) */
void cc_bt_addr(char *out, const volatile unsigned char *a);       /* "AA:BB:CC:DD:EE:FF" */
int  cc_bt_parse_addr(const char *s, unsigned char *a);            /* the reverse: 0 or -1 */
const char *cc_bt_kind(unsigned flags);     /* a paired device's kind */
const char *cc_bt_found_kind(unsigned cod); /* a found one's, from its class of device */
const char *cc_bt_state(unsigned state);
#endif

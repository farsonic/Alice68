/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68's cmdline.txt options: the table and the logic shared by the kernel's
 * setup screen (alice_setup.c) and the Amiga's Alice68-Card (amiga/card.c), so
 * both offer the same settings and write them the same way. Host-independent,
 * no C library: it builds for the kernel, the 68k and the PC tests.
 *
 * opt_init() reads the options from a command line (bootargs or cmdline.txt);
 * opt_edits() gives what changed as an edit list (amiga/hdf/cmdedit.h):
 *     -NAME     drop the options called NAME
 *     +OPTION   add OPTION
 */
#ifndef ALICE_OPTS_H
#define ALICE_OPTS_H
#include <stdint.h>

#define OPT_MAX   24                    /* items, at most */
#define OPT_TEXT  64                    /* a text option's value, with its NUL */
#define OPT_NSEC  4                     /* Display, Storage, Network, USB */

enum { OPT_CHOICE, OPT_TEXTV };
enum { OT_PLAIN = 0, OT_QUOTE = 1, OT_UNDERSCORE = 2, OT_UPPER = 4 };

struct opt_item {
    uint8_t sec, kind, def, flags, maxlen, secret;
    const char *label, *help;
    const char *keys;                   /* choices: the options it owns (space-separated names) */
    const char *const *names;           /* choices: as shown, 0-terminated */
    const char *const *opts;            /* choices: the option each writes ("" none) */
    const char *key;                    /* text: the option's name */
};
extern const struct opt_item opt_items[];
extern const int opt_count;
extern const char *const opt_sections[OPT_NSEC];

struct opt_vals {
    uint8_t cur[OPT_MAX], orig[OPT_MAX];              /* choices */
    char text[OPT_MAX][OPT_TEXT], otext[OPT_MAX][OPT_TEXT];
};

void opt_init(struct opt_vals *v, const char *cmdline);
int  opt_nchoices(const struct opt_item *it);
int  opt_item_changed(const struct opt_vals *v, int i);
int  opt_changed(const struct opt_vals *v);
/* the edit list for what changed; its length, 0 nothing changed, -1 too long */
int  opt_edits(const struct opt_vals *v, char *out, unsigned max);
/* the option as it would be written ("aga.scale=4", "hdf.boot=Work_bench"), "" for none */
void opt_written(const struct opt_vals *v, int i, char *out, unsigned max);
#endif

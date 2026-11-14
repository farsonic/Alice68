/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: the setup screen's changes (src/raspi/alice_setup.c, handed over on the
 * network board) applied to cmdline.txt's text. The edit list has one line per change:
 *     -NAME     drop every option called NAME ("hdf" drops hdf=off; "hdf.readonly" itself)
 *     +OPTION   add OPTION at the end
 * Every other option stays as it was, in its order; the result is one line ending in
 * '\n' (cmdline.txt must be one line). Host-independent: tools/hdf-test tests it.
 */
#ifndef ALICE68_CMDEDIT_H
#define ALICE68_CMDEDIT_H
#include <stdint.h>

/* out gets the new cmdline.txt; its length, or -1 if it would not fit in max */
int cmd_apply(const char *old, const char *edits, char *out, uint32_t max);
#endif

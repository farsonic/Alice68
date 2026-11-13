/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 folders: Amiga file attributes on FAT and exFAT, the way FS-UAE and
 * Amiberry keep them, so a folder works the same in those emulators and on
 * Alice68 (and can be copied between them on a PC):
 *
 *   - a sidecar file NAME.uaem next to NAME, one text line:
 *         hsparwed YYYY-MM-DD HH:MM:SS.CC comment
 *     protection flags (h s p a set when shown; r w e d allowed when shown,
 *     '-' otherwise), the date to 1/100 s, the file comment (up to 79 chars);
 *     an optional UTF-8 byte order mark before it. Written only when FAT/exFAT
 *     cannot hold what the Amiga set (a comment, flags other than ----rwed /
 *     ----r-e-);
 *   - Amiga names with characters the PC forbids stored as %xx (lowercase hex):
 *     % \ * ? " / | < > and control characters, a trailing dot or space,
 *     Windows device names (CON, AUX, PRN, NUL, COM1-9, LPT1-9: the third
 *     letter), and a final ".uaem" (its last letter);
 *   - sidecars and other machines' clutter (._*, .DS_Store, _UAEFSDB.___)
 *     hidden from the Amiga.
 *
 * Written from FS-UAE's documentation of the format; host-independent (no
 * AmigaOS), so tools/hdf-test tests it on a PC.
 */
#ifndef ALICE68_UAEM_H
#define ALICE68_UAEM_H
#include <stdint.h>

#define UAEM_COMMENT 80                  /* 79 characters and the NUL */
#define UAEM_LINE    128                 /* a sidecar's line, at most */

struct uaem {
    uint32_t prot;                       /* fib_Protection: bits 7..4 HSPA set = flag, 3..0 RWED set = not allowed */
    uint32_t days, mins, ticks;          /* AmigaDOS date */
    char     comment[UAEM_COMMENT];
};

/* a host entry the Amiga never sees */
int  uaem_hidden(const char *host);
/* an Amiga name as the host file name: its length, or -1 if it does not fit in max-1 */
int  uaem_encode(const char *amiga, uint32_t len, char *host, uint32_t max);
/* a host name as the Amiga sees it (%xx undone); out may be host */
void uaem_decode(const char *host, char *out, uint32_t max);
/* the sidecar's name for a host name: its length, or -1 */
int  uaem_sidecar(const char *host, char *out, uint32_t max);
/* a sidecar's text: 0 parsed, -1 not a sidecar line */
int  uaem_parse(const uint8_t *text, uint32_t n, struct uaem *m);
/* the line for m (with '\n'); its length */
uint32_t uaem_format(const struct uaem *m, char *out);
/* FAT/exFAT alone can hold these flags (no sidecar needed for them) */
int  uaem_plain(uint32_t prot);
#endif

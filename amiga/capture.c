/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68Prefs's Status, Joystick, Logs and Monitor pages show what `Alice68 STATUS`,
 * `JOYTEST ONCE`, `LOG` and `MONITOR DUMP` print, by running the same code with its output
 * caught here. The Shell command's tools all print through lvo.S's dos_write(); in
 * Alice68Prefs that symbol is renamed dos_write_real (build.sh) and this dos_write takes its
 * place: while a capture runs it keeps the text, otherwise it writes as before.
 */
#include "amiga.h"

LONG dos_write_real(APTR dos, ULONG fh, const void *buf, LONG len);

static char *cap_buf;
static ULONG cap_len, cap_max;
static int cap_on;

LONG dos_write(APTR dos, ULONG fh, const void *buf, LONG len)
{
    if (!cap_on) return dos_write_real(dos, fh, buf, len);
    const char *s = buf;
    for (LONG i = 0; i < len && cap_len + 1 < cap_max; i++) cap_buf[cap_len++] = s[i];
    cap_buf[cap_len] = 0;
    return len;
}

#define PR_COS 160
/* fn's output (args as on its command line) into buf, NUL-terminated; fn's return code */
LONG a68_capture(LONG (*fn)(const char *, ULONG), const char *args, char *buf, ULONG max)
{
    UBYTE *me = (UBYTE *)lvo(SYSBASE, -294, 0, 0, 0, 0);      /* FindTask(NULL) */
    ULONG cos = *(ULONG *)(me + PR_COS);
    ULONG n = 0;
    while (args[n]) n++;
    cap_buf = buf; cap_len = 0; cap_max = max; cap_on = 1;
    buf[0] = 0;
    if (!cos) *(ULONG *)(me + PR_COS) = 1;                      /* from Workbench: no Output(), and the tools would not write */
    LONG rc = fn(args, n);
    *(ULONG *)(me + PR_COS) = cos;
    cap_on = 0;
    return rc;
}

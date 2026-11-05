/* mkicon - make a package icon from a system default icon.
 *
 *   mkicon TEMPLATE DEST [TOOL=deftool] [STACK=n] [TT tooltype ...]   write DEST.info
 *   mkicon TEMPLATE INFO                                     describe TEMPLATE.info
 *
 * The icon itself comes from TEMPLATE (e.g. ENVARC:Sys/def_Installer), read and
 * written through icon.library, so colour icons survive intact - which a
 * hand-built .info on the PC would not guarantee. The position is cleared so
 * Workbench places the icon itself.
 * Build: m68k-amigaos-gcc -O2 -noixemul -o mkicon mkicon.c
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <string.h>

struct Library *IconBase;

int main(int argc, char **argv)
{
    if (argc < 3) {
        PutStr("usage: mkicon TEMPLATE DEST [TOOL=deftool] [TT tooltype ...] | mkicon TEMPLATE INFO\n");
        return 5;
    }
    IconBase = OpenLibrary((STRPTR)"icon.library", 36);
    if (!IconBase) return 20;
    struct DiskObject *d = GetDiskObject((STRPTR)argv[1]);
    if (!d) { Printf("mkicon: cannot read %s.info\n", (ULONG)argv[1]); CloseLibrary(IconBase); return 10; }

    if (!strcmp(argv[2], "INFO")) {
        Printf("type %ld, default tool \"%s\", stack %ld\n", (LONG)d->do_Type,
               (ULONG)(d->do_DefaultTool ? (char *)d->do_DefaultTool : ""), d->do_StackSize);
        for (char **t = (char **)d->do_ToolTypes; t && *t; t++) Printf("  tooltype \"%s\"\n", (ULONG)*t);
        FreeDiskObject(d);
        CloseLibrary(IconBase);
        return 0;
    }

    STRPTR old_tool = d->do_DefaultTool;
    STRPTR *old_tt = d->do_ToolTypes;
    LONG old_x = d->do_CurrentX, old_y = d->do_CurrentY;
    static char *tt[16];
    int ntt = 0, in_tt = 0;
    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "TOOL=", 5)) d->do_DefaultTool = (STRPTR)argv[i] + 5;
        else if (!strncmp(argv[i], "STACK=", 6)) {
            LONG s = 0;
            for (const char *p = argv[i] + 6; *p >= '0' && *p <= '9'; p++) s = s * 10 + (*p - '0');
            if (s >= 4096) d->do_StackSize = s;
        }
        else if (!strcmp(argv[i], "TT")) in_tt = 1;
        else if (in_tt && ntt < 15) tt[ntt++] = argv[i];
    }
    if (in_tt) { tt[ntt] = NULL; d->do_ToolTypes = (STRPTR *)tt; }
    d->do_CurrentX = NO_ICON_POSITION;
    d->do_CurrentY = NO_ICON_POSITION;
    BOOL ok = PutDiskObject((STRPTR)argv[2], d);
    /* hand the template's own pointers back before it is freed */
    d->do_DefaultTool = old_tool;
    d->do_ToolTypes = old_tt;
    d->do_CurrentX = old_x;
    d->do_CurrentY = old_y;
    FreeDiskObject(d);
    CloseLibrary(IconBase);
    Printf("mkicon: %s.info %s\n", (ULONG)argv[2], (ULONG)(ok ? "written" : "FAILED"));
    return ok ? 0 : 10;
}

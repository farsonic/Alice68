/* SPDX-License-Identifier: GPL-2.0-only
 * Alice68-HDF - show alice68hdf.device's hardfiles and its log, in a Shell. */
#include "../amiga.h"
#include "hdf_core.h"

void *memcpy(void *d, const void *s, unsigned long n) { UBYTE *a = d; const UBYTE *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { UBYTE *a = d; while (n--) *a++ = (UBYTE)c; return d; }

static APTR dos; static ULONG out;
static void puts_(const char *s) { LONG n = 0; while (s[n]) n++; Write(dos, out, s, n); }
static void dec(ULONG v) { char b[12]; int n = 11; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + n); }

/* the start of alice68hdf.c's struct HdfBase */
struct HdfHead { struct Library lib; UWORD pad; ULONG magic, nunits; APTR unit[HDF_MAX_FILES]; ULONG log_wr; char log[4096]; };
struct HUnitHead { struct Unit u; struct hdf_file f; };

LONG hdfinfo_main(const char *args, ULONG arglen)
{
    (void)args; (void)arglen;
    dos = OpenLibrary("dos.library", 36);
    if (!dos) return 20;
    out = Output(dos);
    Forbid();
    struct HdfHead *db = FindName((UBYTE *)SYSBASE + EXEC_DEVICELIST, "alice68hdf.device");
    Permit();
    if (!db || db->magic != 0x41363848u) {
        puts_("alice68hdf.device is not running (not an Alice68 kernel with hardfile support?)\n");
        CloseLibrary(dos);
        return 10;
    }
    puts_("alice68hdf.device: "); dec(db->nunits); puts_(db->nunits == 1 ? " unit\n" : " units\n");
    for (ULONG i = 0; i < db->nunits && i < HDF_MAX_FILES; i++) {
        struct HUnitHead *u = db->unit[i];
        if (!u) continue;
        puts_("  unit "); dec(i); puts_(": "); puts_(u->f.name); puts_(", "); dec(u->f.size >> 20);
        puts_(" MB, "); dec(u->f.next); puts_(u->f.next == 1 ? " piece\n" : " pieces\n");
    }
    puts_("\n--- log ---\n");
    ULONG wr = db->log_wr, start = wr > 4096 ? wr - 4096 : 0;
    char buf[256]; ULONG n = 0;
    for (ULONG i = start; i < wr; i++) {
        buf[n++] = db->log[i % 4096];
        if (n == sizeof buf) { Write(dos, out, buf, (LONG)n); n = 0; }
    }
    if (n) Write(dos, out, buf, (LONG)n);
    CloseLibrary(dos);
    return 0;
}

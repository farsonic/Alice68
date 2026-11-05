/* ahitest - aga-hdmi.audio from a Shell, step by step: my mode's name from
 * AHI's database, AHI_AllocAudio with it, a 1 kHz tone played for three
 * seconds, and the kernel's AHI counter (511) before, during and after.
 *   ahitest [AUDIOID [RATE]]   (default $0E680002, the HiFi mode, mixed at 48000 Hz)
 * Build: tools/ahi/build.sh. SPDX-License-Identifier: GPL-2.0-only
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/ahi.h>
#include <stdio.h>
#include <stdlib.h>

struct Library *AHIBase;

#define DIAG_SEL (*(volatile UWORD *)0xDFF1E8)
#define DIAG_DAT (*(volatile UWORD *)0xDFF1EA)
static ULONG diag(UWORD n)
{
    UWORD sel = n >= 256 ? (UWORD)(0x1700 + (n - 256)) : n;
    Disable();
    DIAG_SEL = sel;
    ULONG hi = DIAG_DAT, lo = DIAG_DAT;
    Enable();
    return hi << 16 | lo;
}

static void show(const char *when)
{
    static const char *const r[6] = { "44.1", "48", "88.2", "96", "176.4", "192" };
    ULONG v = diag(511), b = diag(147), s = diag(490);
    printf("  kernel %s: built for Pi %lu, HDMI state %lu, AHI understood %lu ring %lu HDMI-running %lu waiting %lu empty %lu\n",
           when, b, s, v & 1, (v >> 1) & 1, (v >> 2) & 1, ((v >> 16) & 255) * 64, v >> 24);
    printf("    screen takes:");
    for (int i = 0; i < 6; i++) if ((v >> (4 + i)) & 1) printf(" %s", r[i]);
    printf(" kHz, %s bit; HDMI at %s kHz; ring frames %s bit\n", (v & 8) ? "24" : "16",
           r[((v >> 10) & 7) < 6 ? (v >> 10) & 7 : 1], (v & (1UL << 13)) ? "32" : "16");
}

int main(int argc, char **argv)
{
    ULONG id = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x0E680002;
    ULONG freq = argc > 2 ? strtoul(argv[2], NULL, 0) : 48000;      /* the mixing rate asked for */
    static WORD tone[48000];
    for (int i = 0; i < 48000; i++) {
        int p = i % 48;                                  /* 1 kHz: a triangle, 48 samples a period */
        int v = p < 24 ? p * 2 - 24 : 72 - p * 2;       /* -24..24 */
        tone[i] = (WORD)(v * 1000);
    }
    show("before");
    struct MsgPort *mp = CreateMsgPort();
    struct AHIRequest *io = mp ? (struct AHIRequest *)CreateIORequest(mp, sizeof(struct AHIRequest)) : NULL;
    if (!io) { printf("no memory\n"); return 20; }
    io->ahir_Version = 4;
    BYTE err = OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)io, 0);
    printf("OpenDevice(ahi.device, NO_UNIT): %d\n", err);
    if (err) return 20;
    AHIBase = (struct Library *)io->ahir_Std.io_Device;
    char name[80] = "?", driver[40] = "?";
    LONG ok = AHI_GetAudioAttrs(id, NULL, AHIDB_Name, (ULONG)name, AHIDB_Driver, (ULONG)driver, AHIDB_BufferLen, 40, TAG_DONE);
    printf("mode $%08lx in AHI's database: %s - \"%s\", driver \"%s\"\n", id, ok ? "yes" : "NO", name, driver);
    struct AHIAudioCtrl *ac = AHI_AllocAudio(AHIA_AudioID, id, AHIA_MixFreq, freq, AHIA_Channels, 1, AHIA_Sounds, 1, TAG_DONE);
    printf("AHI_AllocAudio: %s\n", ac ? "ok" : "FAILED");
    if (ac) {
        ULONG mix = 0;
        AHI_ControlAudio(ac, AHIC_MixFreq_Query, (ULONG)&mix, TAG_DONE);
        printf("  mixing at %lu Hz\n", mix);
        struct AHISampleInfo si = { AHIST_M16S, tone, 48000 };
        ULONG e = AHI_LoadSound(0, AHIST_SAMPLE, &si, ac);
        printf("AHI_LoadSound: %lu\n", e);
        e = AHI_ControlAudio(ac, AHIC_Play, TRUE, TAG_DONE);
        printf("AHI_ControlAudio(Play): %lu\n", e);
        AHI_Play(ac, AHIP_BeginChannel, 0, AHIP_Freq, 48000, AHIP_Vol, 0x10000, AHIP_Pan, 0x8000,
                 AHIP_Sound, 0, AHIP_EndChannel, 0, TAG_DONE);
        printf("playing a 1 kHz tone for 3 s...\n");
        Delay(50);
        show("playing");
        Delay(100);
        show("playing");
        AHI_ControlAudio(ac, AHIC_Play, FALSE, TAG_DONE);
        AHI_FreeAudio(ac);
        show("after");
    }
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(mp);
    return 0;
}

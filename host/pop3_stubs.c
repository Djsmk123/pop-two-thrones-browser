/* Prince of Persia: The Two Thrones: imports with no real implementation yet.
 * Bink movies fail to open (the game skips them), EAX is absent (Direct3D 9: d3d9.c).
 */
#include "win32.h"

#define W(name) static void w_##name(CPU *c)

W(ShellExecuteA) { STUB("ShellExecuteA(%s)\n", ARG(2) ? GSTR(ARG(2)) : ""); RET(6, 33); }
W(waveOutGetDevCapsA) { RET(3, 2 /* MMSYSERR_BADDEVICEID */); }

void w_DirectInput8Create(CPU *c);  /* dinput.c */

/* Bink: movies are not decoded. BinkOpen hands back a short black two-frame movie so the game's player runs to its end
   instead of waiting on a movie that never starts. HBINK: +0 Width, +4 Height, +8 Frames, +12 FrameNum, +16 LastFrameNum,
   +20 FrameRate, +24 FrameRateDiv (the fields the game reads). */
#define BINK_FRAMES 2
#define BLOG(n) do { static int cnt; if (g_log && cnt++ < 12) fprintf(stderr, "[bink] %s(%08x, %08x, %08x) frame=%u/%u\n", n, ARG(0), ARG(1), ARG(2), ARG(0) ? R32(ARG(0) + 12) : 0, ARG(0) ? R32(ARG(0) + 8) : 0); } while (0)
W(BinkOpen) {
    uint32_t b = heap_alloc(256, 1);
    W32(b, 640); W32(b + 4, 480); W32(b + 8, BINK_FRAMES); W32(b + 12, 1); W32(b + 16, 0); W32(b + 20, 30); W32(b + 24, 1);
    LOG("[stub] BinkOpen(%s) -> fake 2-frame movie %08x\n", ARG(0) ? GSTR(ARG(0)) : "", b);
    RET(2, b);
}
W(BinkClose) { BLOG("BinkClose"); if (ARG(0)) heap_free(ARG(0)); RET(1, 0); }
W(BinkDoFrame) { BLOG("BinkDoFrame"); RET(1, 0); }
W(BinkNextFrame) { BLOG("BinkNextFrame");
    uint32_t b = ARG(0);
    if (b) { W32(b + 16, R32(b + 12)); uint32_t f = R32(b + 12); if (f < BINK_FRAMES) W32(b + 12, f + 1); }
    RET(1, 0);
}
W(BinkWait) { BLOG("BinkWait"); RET(1, 0); }   /* 0: the next frame is due */
W(BinkGoto) { BLOG("BinkGoto"); if (ARG(0)) W32(ARG(0) + 12, ARG(1)); RET(3, 0); }
W(BinkPause) { BLOG("BinkPause"); RET(2, 0); }
W(BinkSetVolume) { RET(3, 0); }
W(BinkSetSoundTrack) { RET(2, 0); }
W(BinkSetSoundSystem) { RET(2, 0); }
W(BinkOpenDirectSound) { RET(1, 0); }
W(BinkCopyToBuffer) { BLOG("BinkCopyToBuffer"); RET(7, 0); }

#define B(name, n) { "BINKW32.DLL!_" #name "@" #n, w_##name }
static const ImportHandler list[] = {
    { "SHELL32.DLL!ShellExecuteA", w_ShellExecuteA },
    { "WINMM.DLL!waveOutGetDevCapsA", w_waveOutGetDevCapsA },
    { "EAX.DLL!#6", NULL },      /* EAXDirectSoundCreate(guid, &ds, outer): same as DirectSoundCreate; patched below */
    { "DINPUT8.DLL!DirectInput8Create", w_DirectInput8Create },
    { "DSOUND.DLL!#11", NULL },  /* DirectSoundCreate8; patched below */
    B(BinkOpen, 8), B(BinkClose, 4), B(BinkDoFrame, 4), B(BinkNextFrame, 4), B(BinkWait, 4), B(BinkGoto, 12),
    B(BinkPause, 8), B(BinkSetVolume, 12), B(BinkSetSoundTrack, 8), B(BinkSetSoundSystem, 8),
    B(BinkOpenDirectSound, 4), B(BinkCopyToBuffer, 28),
};
void w_DirectSoundCreate(CPU *c);  /* dsound.c */
static ImportHandler g_list[sizeof list / sizeof list[0]];
static ImportTable make(void) {
    for (size_t i = 0; i < sizeof list / sizeof list[0]; i++) {
        g_list[i] = list[i];
        if (!g_list[i].fn) g_list[i].fn = w_DirectSoundCreate;
    }
    return (ImportTable){ g_list, sizeof list / sizeof list[0] };
}
const ImportTable *pop3_imports(void) { static ImportTable t; if (!t.list) t = make(); return &t; }

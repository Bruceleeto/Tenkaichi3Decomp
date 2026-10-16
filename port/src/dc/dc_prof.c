/* BT3_PROF (make -f Makefile.dc DC_PROF=1): the whole program profiled on the Dreamcast, by sampling. At each tick
   of the scheduler's timer (thd_set_hz: 1000 times a second; timer_primary_set_callback, chained in front of the
   scheduler) the program counter it interrupted is counted in a histogram over the program's code (32-byte bins: about 85 KB, taken from KOS's malloc when the first frame
   ends, not from the game's heap); no -pg, nothing instrumented, every function is seen (the game's, the port's,
   KOS's: thd_idle_task is the time nothing ran). Every 20 seconds the
   bins that were hit are printed as "prof:" lines with the fps line (Gs_FramePace) and the histogram starts again;
   port/tools/dc_prof.py turns a log of them into functions. A sample inside newlib's memcpy or memset (leaf functions:
   pr is still the return address) also counts its caller, kept exactly in a small table, printed as "prof: c" lines. */
#if defined(__DREAMCAST__) && defined(BT3_PROF)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos/thread.h>
#include <arch/irq.h>
#include <arch/timer.h>

#define PROF_BIN 32u
#define PROF_HZ 1000u
#define PROF_EVERY_MS 20000u
#define PROF_CALLERS 512u /* (a power of two) */

extern char sProfLo __asm__("__executable_start");
extern char sProfHi __asm__("__etext");

static uint16_t *sHist;
static uint32_t sBins;
static volatile int sOn;
static volatile uint32_t sSamples, sOutside, sFull;
static uint64_t sFrom;
static unsigned sDump;

/* the copies' callers: newlib's memcpy and memset lie together ([memcpy, strcpy) in the link, checked at the start) */
static uintptr_t sCopyLo, sCopyHi;
static uint32_t sCallPr[PROF_CALLERS], sCallN[PROF_CALLERS];
static volatile uint32_t sCopies, sCallLost;
static void count_caller(uint32_t pr) {
    uint32_t i = (pr >> 1) * 2654435761u >> 23 & (PROF_CALLERS - 1), k;
    sCopies++;
    for (k = 0; k < 16; k++, i = (i + 1) & (PROF_CALLERS - 1)) {
        if (sCallPr[i] == pr) { sCallN[i]++; return; }
        if (sCallPr[i] == 0) { sCallPr[i] = pr; sCallN[i] = 1; return; }
    }
    sCallLost++;
}

/* (the primary timer's interrupt, the scheduler's tick: ctx is what it interrupted; then the scheduler as before) */
static timer_primary_callback_t sNext;
static void prof_tick(irq_context_t *ctx) {
    if (sOn) {
        uintptr_t pc = CONTEXT_PC(*ctx);
        sSamples++;
        if (pc >= (uintptr_t)&sProfLo && pc < (uintptr_t)&sProfHi) {
            uint16_t *h = &sHist[(pc - (uintptr_t)&sProfLo) / PROF_BIN];
            if (*h != 0xFFFF) { (*h)++; } else { sFull++; }
            if (pc >= sCopyLo && pc < sCopyHi) { count_caller(ctx->pr); }
        } else {
            sOutside++;
        }
    }
    if (sNext != NULL) { sNext(ctx); }
}

static void prof_start(void) {
    sBins = (uint32_t)(((uintptr_t)&sProfHi - (uintptr_t)&sProfLo + PROF_BIN - 1) / PROF_BIN);
    sHist = calloc(sBins, sizeof(*sHist));
    if (sHist == NULL) {
        printf("prof: ERROR: no %u bytes for the histogram: not profiled\n", (unsigned)(sBins * sizeof(*sHist)));
        sBins = 0;
        return;
    }
    sCopyLo = (uintptr_t)&memcpy;
    sCopyHi = (uintptr_t)&strcpy;
    if (!((uintptr_t)&memset > sCopyLo && (uintptr_t)&memset < sCopyHi && sCopyHi - sCopyLo < 0x400)) {
        printf("prof: ERROR: memcpy %08x, memset %08x, strcpy %08x not together: the copies' callers not counted\n",
               (unsigned)sCopyLo, (unsigned)(uintptr_t)&memset, (unsigned)sCopyHi);
        sCopyLo = sCopyHi = 0;
    }
    thd_set_hz(PROF_HZ);
    sNext = timer_primary_set_callback(prof_tick);
    if (sNext == NULL) {
        printf("prof: ERROR: the primary timer had no callback (not the scheduler's tick?): not profiled\n");
        timer_primary_set_callback(NULL);
        free(sHist);
        sHist = NULL;
        sBins = 0;
        return;
    }
    printf("prof: on: %u Hz, code %08x..%08x in %u bins of %u bytes (%u KB)\n", thd_get_hz(), (unsigned)(uintptr_t)&sProfLo,
           (unsigned)(uintptr_t)&sProfHi, (unsigned)sBins, PROF_BIN, (unsigned)(sBins * sizeof(*sHist) / 1024));
    sFrom = timer_ms_gettime64();
    sOn = 1;
}

/* Each frame (gs/gs_walk.c Gs_FramePace): the histogram printed and begun again every PROF_EVERY_MS. */
void Prof_Frame(void) {
    static int started;
    uint64_t now;
    uint32_t i, k = 0;

    if (!started) {
        started = 1;
        prof_start();
    }
    if (sHist == NULL) { return; }
    now = timer_ms_gettime64();
    if (now - sFrom < PROF_EVERY_MS) { return; }
    sOn = 0;
    printf("prof: dump %u: %u samples in %u ms, %u outside the code, %u in full bins\n", sDump, (unsigned)sSamples,
           (unsigned)(now - sFrom), (unsigned)sOutside, (unsigned)sFull);
    for (i = 0; i < sBins; i++) { /* the bins hit, as offset=count, 10 a line */
        if (sHist[i] == 0) { continue; }
        if (k % 10 == 0) { printf(k == 0 ? "prof: b" : "\nprof: b"); }
        printf(" %x=%u", (unsigned)(i * PROF_BIN), (unsigned)sHist[i]);
        k++;
    }
    printf("\nprof: copies %u samples in memcpy / memset, %u callers not kept\n", (unsigned)sCopies, (unsigned)sCallLost);
    for (i = 0, k = 0; i < PROF_CALLERS; i++) { /* their callers, as address=count, 10 a line */
        if (sCallPr[i] == 0) { continue; }
        if (k % 10 == 0) { printf(k == 0 ? "prof: c" : "\nprof: c"); }
        printf(" %x=%u", (unsigned)sCallPr[i], (unsigned)sCallN[i]);
        k++;
    }
    printf("\nprof: end %u\n", sDump);
    fflush(stdout);
    memset(sHist, 0, sBins * sizeof(*sHist));
    memset(sCallPr, 0, sizeof(sCallPr));
    memset(sCallN, 0, sizeof(sCallN));
    sSamples = sOutside = sFull = sCopies = sCallLost = 0;
    sDump++;
    sFrom = timer_ms_gettime64();
    sOn = 1;
}
#endif

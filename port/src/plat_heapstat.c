/*
 * The game heap's use, for fitting the game into less memory (the Dreamcast has 16 MB in all).
 *
 *   BT3_HEAP_MB=<n>     the game heap is n MB instead of the console's 27 (src/sys/heap.c, Heap_Create)
 *   BT3_HEAP_REPORT=1   the report without a cap
 *
 * With either: a line each time the peak grows by another MB, the largest users at the last such line (by the
 * code that called Heap_Alloc), and a summary at exit. Always, with or without them: a line for an allocation the
 * heap could not serve, with what asked and how much.
 *
 * Two measures, because the heap is first-fit from its head and (some blocks) from its tail:
 *   peak live   the most bytes the game held at once: no heap can be smaller
 *   span        the head side's highest end plus the tail side's deepest start, as far as they ever reached: a heap
 *               this large serves the same allocations at the same places (relative to its two ends), so it is
 *               enough; the size to give BT3_HEAP_MB lies between the two
 *
 * The caller addresses are this program's (not relocated): `addr2line -f -e port/build/bt3 0x...` names one;
 * the report names those it can through the dynamic symbol table (the program is linked with -rdynamic).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS (1 << 16) /* live allocations tracked (open addressing on the data address) */
#define TOP 12

typedef struct Live {
    uint32_t addr, size;
    uintptr_t caller;
} Live;

static Live *sLive; /* SLOTS of them, taken when the report is switched on (no memory used otherwise) */
static uint32_t sLiveCount, sDropped;
static int sOn;
static uint32_t sBase, sSize;
static uint32_t sUsed, sPeak, sNextNote, sAllocs, sFails;
static uint32_t sHeadEnd, sTailStart; /* offsets from sBase: the highest end of a head block, the lowest start of a tail one */
static struct { uintptr_t caller; uint32_t bytes, n; } sTop[TOP];
static int sTopCount;
static uint32_t sTopAt;

extern int Heap_GetFreeSize(void);
extern unsigned Heap_GetLargestFree(void);

/* The heap's size: the console's unless BT3_HEAP_MB asks for less (or more). */
int Port_HeapCap(int size) {
    const char *mb = getenv("BT3_HEAP_MB");
    if (mb != NULL && atoi(mb) > 0) {
        size = atoi(mb) << 20;
    }
    return size;
}

/* glibc's dladdr, declared here: this file is compiled with stdlib.h included first (undefined.py, the Makefile),
   which is too early for _GNU_SOURCE to make <dlfcn.h> declare it. Elsewhere only the address is printed. */
#if defined(__linux__)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } PortDlInfo;
extern int dladdr(const void *addr, PortDlInfo *info);
#endif

static const char *name_of(uintptr_t addr, char *buf, size_t n) {
#if defined(__linux__)
    PortDlInfo info;
    if (dladdr((void *)addr, &info) && info.dli_sname != NULL) {
        snprintf(buf, n, "%s+0x%x (0x%08x)", info.dli_sname, (unsigned)(addr - (uintptr_t)info.dli_saddr), (unsigned)addr);
        return buf;
    }
#endif
    snprintf(buf, n, "0x%08x", (unsigned)addr);
    return buf;
}

static uint32_t span(void) {
    return sHeadEnd + (sTailStart < sSize ? sSize - sTailStart : 0);
}

/* Who holds the most right now, summed by caller. */
static void snapshot(void) {
    enum { CALLERS = 1024 };
    static struct { uintptr_t caller; uint32_t bytes, n; } by[CALLERS];
    int count = 0, i, j;

    if (sLive == NULL) { return; }
    for (i = 0; i < SLOTS; i++) {
        if (sLive[i].addr == 0) { continue; }
        for (j = 0; j < count && by[j].caller != sLive[i].caller; j++) {}
        if (j == count) {
            if (count == CALLERS) { continue; }
            by[count].caller = sLive[i].caller;
            by[count].bytes = by[count].n = 0;
            count++;
        }
        by[j].bytes += sLive[i].size;
        by[j].n++;
    }
    sTopCount = 0;
    for (i = 0; i < TOP && i < count; i++) {
        int best = i;
        for (j = i + 1; j < count; j++) {
            if (by[j].bytes > by[best].bytes) { best = j; }
        }
        { __typeof__(by[0]) t = by[i]; by[i] = by[best]; by[best] = t; }
        sTop[i].caller = by[i].caller;
        sTop[i].bytes = by[i].bytes;
        sTop[i].n = by[i].n;
        sTopCount++;
    }
    sTopAt = sUsed;
}

static void report(void) {
    char buf[160];
    int i;

    fprintf(stderr, "bt3: heap: %u KB; peak live %u KB, span %u KB; %u allocations, %u failed%s\n", sSize >> 10, sPeak >> 10,
            span() >> 10, sAllocs, sFails, sDropped ? " (some not tracked: table full)" : "");
    if (sTopCount > 0) {
        fprintf(stderr, "bt3: heap: the largest users at %u KB live:\n", sTopAt >> 10);
        for (i = 0; i < sTopCount; i++) {
            fprintf(stderr, "bt3: heap:   %7u KB in %4u blocks  %s\n", sTop[i].bytes >> 10, sTop[i].n, name_of(sTop[i].caller, buf, sizeof(buf)));
        }
    }
}

void Port_HeapCreated(unsigned start, int size) {
    static int once;
    if (sLive != NULL) {
        memset(sLive, 0, SLOTS * sizeof(*sLive));
    }
    sLiveCount = sDropped = sUsed = sPeak = sAllocs = sFails = 0;
    sBase = start;
    sSize = (uint32_t)size;
    sHeadEnd = 0;
    sTailStart = sSize;
    sNextNote = 1u << 20;
    sTopCount = 0;
    sOn = getenv("BT3_HEAP_MB") != NULL || getenv("BT3_HEAP_REPORT") != NULL;
    if (sOn && sLive == NULL && (sLive = calloc(SLOTS, sizeof(*sLive))) == NULL) {
        fprintf(stderr, "bt3: ERROR: no memory for the heap report (%u KB): it is off\n", (unsigned)(SLOTS * sizeof(*sLive)) >> 10);
        sOn = 0;
    }
    if (sOn) {
        fprintf(stderr, "bt3: heap: %u KB at 0x%08x%s\n", sSize >> 10, sBase, getenv("BT3_HEAP_MB") != NULL ? " (BT3_HEAP_MB)" : "");
        if (!once) { atexit(report); }
        once = 1;
    }
}

void Port_HeapNoteAlloc(unsigned data, int size, unsigned align, int fromTail, unsigned block, int blockSize, void *caller) {
    uint32_t h;

    if (data == 0) {
        char buf[160];
        if (size <= 0) { return; }
        sFails++;
        fprintf(stderr, "bt3: heap: out of memory: %d bytes (align %u, from the %s) asked by %s; %u KB live, %d KB free, "
                "largest free block %u KB, heap %u KB\n", size, align, fromTail ? "tail" : "head",
                name_of((uintptr_t)caller, buf, sizeof(buf)), sUsed >> 10, Heap_GetFreeSize() >> 10, Heap_GetLargestFree() >> 10, sSize >> 10);
        return;
    }
    sAllocs++;
    if (!sOn) { return; }
    if (fromTail) {
        if (block - sBase < sTailStart) { sTailStart = block - sBase; }
    } else if (block - sBase + (uint32_t)blockSize > sHeadEnd) {
        sHeadEnd = block - sBase + (uint32_t)blockSize;
    }
    if (sLiveCount >= SLOTS / 2) { /* (kept at most half full) */
        sDropped++;
        return;
    }
    for (h = (data >> 4) & (SLOTS - 1); sLive[h].addr != 0; h = (h + 1) & (SLOTS - 1)) {}
    sLive[h].addr = data;
    sLive[h].size = (uint32_t)size;
    sLive[h].caller = (uintptr_t)caller;
    sLiveCount++;
    sUsed += (uint32_t)size;
    if (sUsed > sPeak) {
        sPeak = sUsed;
        if (sPeak >= sNextNote) {
            fprintf(stderr, "bt3: heap: peak %u KB live (span %u KB)\n", sPeak >> 10, span() >> 10);
            snapshot();
            while (sNextNote <= sPeak) { sNextNote += 1u << 20; }
        }
    }
}

void Port_HeapNoteFree(unsigned data) {
    uint32_t h, next;

    if (!sOn || data == 0) { return; }
    for (h = (data >> 4) & (SLOTS - 1); sLive[h].addr != 0 && sLive[h].addr != data; h = (h + 1) & (SLOTS - 1)) {}
    if (sLive[h].addr == 0) { return; } /* (one the table had no room for) */
    sUsed -= sLive[h].size;
    sLive[h].addr = 0;
    sLiveCount--;
    /* close the gap: move up the entries of the run that follows, so that lookups still find them */
    for (next = (h + 1) & (SLOTS - 1); sLive[next].addr != 0; next = (next + 1) & (SLOTS - 1)) {
        uint32_t home = (sLive[next].addr >> 4) & (SLOTS - 1);
        if (((next - home) & (SLOTS - 1)) >= ((next - h) & (SLOTS - 1))) {
            sLive[h] = sLive[next];
            sLive[next].addr = 0;
            h = next;
        }
    }
}

/*
 * make dcsim: the PC build with the Dreamcast's memory (port/build/bt3-dcsim). The objects both builds have that
 * allocate (the renderer, the sound, the texture pack, the movie player...) have their malloc, calloc, realloc,
 * free, fopen and fclose renamed to the dcsim_ ones here (port/tools/link.py, BT3_DCSIM); those get the
 * Dreamcast's own malloc (newlib's, dcsim_mallocr.inc) on an arena the size of the RAM the Dreamcast has: from the
 * end of the Dreamcast program (DCSIM_HEAP_START, port/build/dc/bt3.elf's _end) to the top of its RAM less the
 * kernel stack, as KallistiOS gives it out (mm_sbrk). Before the first block: what KallistiOS takes itself
 * (DCSIM_KOS_BYTES) and the game heap (port/src/plat_mem.c map_heap, one malloc on the Dreamcast; the PC maps it
 * at the PS2's address instead). What runs out here runs out there; the PC's own code (the PowerVR emulator, the
 * window, the online play) stays on the system's malloc.
 *
 *   BT3_DCSIM_MB=<n>   as if it had n MB (default 32: the 32 MB mod; the stock console has 16)
 *   BT3_DCSIM_LOG=1    every block of 32 KB or more, with who asked for it (addr2line -e port/build/bt3-dcsim)
 *
 * Not counted: KallistiOS's blocks after the start (estimated in DCSIM_KOS_BYTES), newlib's own beyond the
 * buffer of an open file (charged here at fopen), the C library's blocks made inside it (strdup and the like).
 * After the idea of iortcw's DCSIM (SP/code/sys/dcsim_malloc.c).
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef DCSIM_HEAP_START
#error "DCSIM_HEAP_START: where the Dreamcast program ends (set by the dcsim target of the Makefile)"
#endif
#ifndef DCSIM_KOS_BYTES
#define DCSIM_KOS_BYTES 105472 /* (iortcw's measure on the same KallistiOS: what it mallocs before main) */
#endif
#define DC_RAM_BASE 0x8C000000u
#define KERNEL_STACK (64 * 1024)
#define GAME_HEAP (0x02000000u - 0x003BE000u) /* plat_mem.c HEAP_END - HEAP_BASE */
#define FILE_BYTES (1024 + 112) /* newlib: an open file's buffer (BUFSIZ) and its FILE */

static uintptr_t sTop;           /* as a Dreamcast address */
static unsigned char *sArena;    /* where DCSIM_HEAP_START is */
static uintptr_t sBrk;           /* as a Dreamcast address */
static uintptr_t sBrkPeak;       /* the most RAM taken: the break never comes down */
static size_t sInUse, sPeak;     /* the blocks given out (sizes asked for) */
static int sLog;
static void refused(size_t size, void *from, const char *how);

static void *dcsim_sbrk(ptrdiff_t increment) {
    uintptr_t base = sBrk, next;

    increment = (increment + 3) & ~3;
    next = base + increment;
    if (next >= sTop) {
        static unsigned count;
        if (count++ >= 20) { return (void *)-1; }
        /* (KallistiOS's words) */
        printf("Out of memory. Requested sbrk_base 0x%x, was 0x%x, diff %d\n", (unsigned)next, (unsigned)base, (int)increment);
        fflush(stdout);
        return (void *)-1;
    }
    sBrk = next;
    if (next > sBrkPeak) {
        /* a new high: said at each 512 K (the end report does not come when the program is stopped) */
        if ((next - DC_RAM_BASE) >> 19 != (sBrkPeak - DC_RAM_BASE) >> 19 && sBrkPeak != 0) {
            printf("DCSIM: RAM taken to %.2f MB (with the kernel stack)\n", (next + KERNEL_STACK - DC_RAM_BASE) / 1048576.0);
            fflush(stdout);
        }
        sBrkPeak = next;
    }
    return sArena + (base - DCSIM_HEAP_START);
}

/* the Dreamcast's malloc, its names dl_ */
#define malloc dl_malloc
#define calloc dl_calloc
#define realloc dl_realloc
#define memalign dl_memalign
#define free dl_free
#define cfree dl_cfree
#define valloc dl_valloc
#define pvalloc dl_pvalloc
#define mallinfo dl_mallinfo
#define mallopt dl_mallopt
#define malloc_stats dl_malloc_stats
#define malloc_trim dl_malloc_trim
#define malloc_usable_size dl_malloc_usable_size
#define HAVE_MMAP 0
#define MORECORE dcsim_sbrk
#define MORECORE_CLEARS 0
#define malloc_getpagesize (4096)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#include "dcsim_mallocr.inc"
#pragma GCC diagnostic pop
#undef malloc
#undef calloc
#undef realloc
#undef memalign
#undef free

static int in_arena(const void *p) {
    return sArena != NULL && (const unsigned char *)p >= sArena && (const unsigned char *)p < sArena + (sTop - DCSIM_HEAP_START);
}

static void report(void) {
    uintptr_t need = sBrkPeak + KERNEL_STACK - DC_RAM_BASE;
    printf("DCSIM: at the end %u K of blocks in use (the most %u K); the most RAM taken to 0x%x: the Dreamcast needs %u K (%.2f MB), %d K %s 32 MB\n",
           (unsigned)(sInUse >> 10), (unsigned)(sPeak >> 10), (unsigned)sBrkPeak, (unsigned)(need >> 10), need / 1048576.0,
           (int)(((long)need - 32L * 1048576) / 1024 * (need > 32u * 1048576 ? 1 : -1)), need > 32u * 1048576 ? "over" : "under");
    fflush(stdout);
}
static void on_term(int sig) { /* (timeout's) */
    (void)sig;
    report();
    _exit(124);
}

static void init(void) {
    const char *mb = getenv("BT3_DCSIM_MB");
    size_t size;
    unsigned char *block;

    if (sArena != NULL) {
        return;
    }
    sTop = DC_RAM_BASE + (uintptr_t)(mb != NULL ? atoi(mb) : 32) * 1024 * 1024 - KERNEL_STACK;
    sLog = getenv("BT3_DCSIM_LOG") != NULL;
    if (sTop <= DCSIM_HEAP_START) {
        fprintf(stderr, "DCSIM: no RAM above the program\n");
        exit(2);
    }
    size = sTop - DCSIM_HEAP_START;
    block = aligned_alloc(4096, (size + 8192 + 4095) & ~(size_t)4095);
    if (block == NULL) {
        fprintf(stderr, "DCSIM: no %u bytes for the arena\n", (unsigned)size);
        exit(2);
    }
    sArena = block + (DCSIM_HEAP_START & 4095); /* (the same place in a page as on the Dreamcast) */
    sBrk = DCSIM_HEAP_START;
    printf("DCSIM: %u K above the Dreamcast program (it ends at 0x%x), of RAM to 0x%x\n", (unsigned)(size >> 10),
           (unsigned)DCSIM_HEAP_START, (unsigned)(sTop + KERNEL_STACK));
    if (dl_malloc(DCSIM_KOS_BYTES) == NULL || dl_malloc(GAME_HEAP) == NULL) {
        printf("DCSIM: no room for KallistiOS's blocks and the game heap (%u K)\n", (unsigned)(GAME_HEAP >> 10));
        exit(2);
    }
    printf("DCSIM: KallistiOS %u K and the game heap %u K taken: %u K left\n", (unsigned)(DCSIM_KOS_BYTES >> 10),
           (unsigned)(GAME_HEAP >> 10), (unsigned)((sTop - sBrk) >> 10));
    atexit(report);
    signal(SIGTERM, on_term);
}

/* a block that did not fit: said for the first ones (code that tries again each frame would fill the log) */
static void refused(size_t size, void *from, const char *how) {
    static unsigned count;
    if (count++ < 20) {
        printf("DCSIM: no block of %u bytes for %p%s (%u K in use)%s\n", (unsigned)size, from, how, (unsigned)(sInUse >> 10),
               count == 20 ? "; no more of these are said" : "");
        fflush(stdout);
    }
}

static void *given(void *p, size_t size, void *from) {
    if (p == NULL) {
        refused(size, from, "");
        return NULL;
    }
    sInUse += dl_malloc_usable_size(p);
    if (sInUse > sPeak) {
        sPeak = sInUse;
    }
    if (sLog && size >= 32 * 1024) {
        fprintf(stderr, "DCSIM: + %u K for %p (%u K in use)\n", (unsigned)(size >> 10), from, (unsigned)(sInUse >> 10));
    }
    return p;
}

static void taken_back(void *p) {
    sInUse -= dl_malloc_usable_size(p);
}

void *dcsim_malloc(size_t size) {
    init();
    return given(dl_malloc(size), size, __builtin_return_address(0));
}

void *dcsim_calloc(size_t n, size_t size) {
    init();
    return given(dl_calloc(n, size), n * size, __builtin_return_address(0));
}

/* a block from the system's malloc (made inside the C library) goes back to it */
void dcsim_free(void *p) {
    if (p == NULL) {
        return;
    }
    if (in_arena(p)) {
        taken_back(p);
        dl_free(p);
    } else {
        free(p);
    }
}

void *dcsim_realloc(void *p, size_t size) {
    init();
    if (p != NULL && !in_arena(p)) {
        return realloc(p, size);
    }
    {
        size_t was = p != NULL ? dl_malloc_usable_size(p) : 0;
        void *q = dl_realloc(p, size);

        if (q == NULL) {
            if (size != 0) {
                refused(size, __builtin_return_address(0), " (a realloc)");
            } else {
                sInUse -= was; /* (freed) */
            }
            return NULL;
        }
        sInUse -= was;
        return given(q, size, __builtin_return_address(0));
    }
}

/* An open file: newlib's buffer and FILE, as a block of the arena while it is open. */
enum { MAX_FILES = 64 };
static struct { FILE *f; void *buf; } sFiles[MAX_FILES];

FILE *dcsim_fopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    int i;

    if (f != NULL) {
        init();
        for (i = 0; i < MAX_FILES && sFiles[i].f != NULL; i++) {
        }
        if (i < MAX_FILES) {
            sFiles[i].f = f;
            sFiles[i].buf = given(dl_malloc(FILE_BYTES), FILE_BYTES, __builtin_return_address(0));
        }
    }
    return f;
}

int dcsim_fclose(FILE *f) {
    int i;

    for (i = 0; i < MAX_FILES; i++) {
        if (sFiles[i].f == f && f != NULL) {
            if (sFiles[i].buf != NULL) {
                taken_back(sFiles[i].buf);
                dl_free(sFiles[i].buf);
            }
            sFiles[i].f = NULL;
            break;
        }
    }
    return fclose(f);
}

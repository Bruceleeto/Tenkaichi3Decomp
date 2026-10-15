/*
 * The renderer's front end without the GS: what both builds need whether or not the PS2 reference (gs_core.c: GS
 * memory, the packets, the software rasteriser; gs_vu1.c) is linked. The PC build links the reference too
 * (PORT_GS_REF, from its Makefile) and hands it what only it can do; the Dreamcast build has this file alone.
 *   - which renderer draws, and starting it;
 *   - the count of pictures (gGsFrame);
 *   - the settings read from the environment (Port_GetEnv);
 *   - reading the shown picture back for the game (Gs_StoreImage);
 *   - the queue of the records the game hands over (include/port/rnd.h), drawn at their places in the list;
 *   - the frame's time.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "gs_internal.h"
#include "../../../include/port/rnd.h" /* the records of the queue */

unsigned gGsFrame; /* pictures so far (a frame that is only being re-run is not one) */
#define sFrame gGsFrame
static int sGpu;   /* 1: the GPU back end draws (BT3_GS=gpu) */

#ifdef PORT_GS_REF
int GsRef_Start(int gpu);  /* gs_core.c: the reference's tables and switches, the GPU back end (on its thread) */
int GsRef_Threaded(void);  /* gs_core.c: 1 = the back end runs on the render thread (BT3_GS_THREAD) */
void GsRef_StoreImage(unsigned fbp, unsigned y0, unsigned w, unsigned h, unsigned char *rgb); /* from GS memory */
#endif

/* getenv for the renderer's files (gs_internal.h): remembered per name. The names are string literals, so the
   same call site always passes the same pointer and the search is a few pointer comparisons. */
#undef getenv
const char *Port_GetEnv(const char *name) {
    static struct { const char *name, *value; } known[256];
    static volatile int count;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    const char *value;
    int i, n = count;

    for (i = 0; i < n; i++) {
        if (known[i].name == name) {
            return known[i].value;
        }
    }
    value = getenv(name);
    pthread_mutex_lock(&lock);
    if (count < 256) {
        known[count].name = name;
        known[count].value = value;
        count++; /* published last: a reader never sees a half-written entry */
    }
    pthread_mutex_unlock(&lock);
    return value;
}
/* Each place a variable is asked for remembers its answer (the names are literals; the environment does not
   change while the game runs). Asked in the middle of drawing, the search in Port_GetEnv was 3% of a frame. */
#define getenv(name) (__extension__({ static const char *v_; static int k_; if (!k_) { v_ = Port_GetEnv(name); k_ = 1; } v_; }))

/* Which renderer: BT3_GS = "gpu" (the window), "1" (the software reference), "none" (nothing is drawn: tests).
   Without the variable a release program opens its window, so that it can be started by a double click; a
   developer's build draws nothing, as the checks expect. */
const char *Gs_Mode(void) {
    extern volatile uint32_t gPortDataStripped; /* plat_mem.c: 1 in a release program */
    const char *m = getenv("BT3_GS");

    if (m == NULL) {
        return gPortDataStripped == 1 ? "gpu" : NULL;
    }
    return strcmp(m, "none") == 0 || strcmp(m, "off") == 0 || strcmp(m, "0") == 0 ? NULL : m;
}

/* 1 once a renderer is chosen (the first transfer to the GS starts it); 0: nothing is drawn. */
int Gs_On(void) {
    static int on = -1;

    if (on < 0) {
        on = Gs_Mode() != NULL;
#ifdef PORT_GS_REF
        sGpu = GsRef_Start(on && strcmp(Gs_Mode(), "gpu") == 0);
#else
        sGpu = on && strcmp(Gs_Mode(), "gpu") == 0 && GsGpu_Init();
#endif
    }
    return on;
}

int GsGpu_Enabled(void) {
    return sGpu;
}

/* The picture is finished: counted, and BT3_XFADE_AT=<frame> (testing) asks for the fight's screen cross-fade at
   that frame, as the game does between the shots of an intro, so a run without a window can show it in any fight. */
void Gs_FrameCount(void) {
    static int at = -2;
    if (at == -2) {
        at = getenv("BT3_XFADE_AT") != NULL ? atoi(getenv("BT3_XFADE_AT")) : -1;
    }
    if (at >= 0 && (int)sFrame == at) {
        extern void ScrXfade_RequestCapture(void);
        extern void ScrXfade_Start(int request, uint32_t seconds); /* (the game's float, as its 32 bits) */
        ScrXfade_RequestCapture();
        ScrXfade_Start(1, 0x3F800000u);
    }
    if (!gPortResim) { /* (a frame that is only being re-run is not a frame of the picture; a skipped one is counted) */
        sFrame++;
    }
}

/* The game reads a piece of a frame buffer back (sceGsExecStoreImage; the cross-fade of the fight's scenes takes the
   frame being shown, 512 x 224 at a time). `bp` in units of 64 pixels: 0xE00 a frame buffer (32 a page), 8 a row of
   512. Delivered as 3 bytes a pixel, top row first.
     - software renderer: from its GS memory (gs_core.c);
     - GPU back end: the frame buffer's picture is read from the card once and kept for the calls that follow in the
       same frame (two halves), reduced to the GS's 512 x 448;
     - nothing drawn (tests) or the back end on a thread of its own: black, and the game is not told otherwise. */
void Gs_StoreImage(unsigned bp, unsigned w, unsigned h, unsigned char *rgb) {
    extern int GsGpu_ReadFrame(uint32_t fbp, uint8_t *rgba); /* gs_draw.c */
    static uint8_t kept[512 * 448 * 4];
    static unsigned keptFrame = ~0u, keptFbp = ~0u;
    unsigned fbp = bp / 0xE00 * 0x70, y0 = bp % 0xE00 / 8, x, y;

    if (w > 512) { w = 512; }
    memset(rgb, 0, (size_t)w * h * 3);
    if (getenv("BT3_GS_VERBOSE") != NULL || getenv("BT3_STORE_LOG") != NULL) {
        fprintf(stderr, "gs: frame %u: the game reads %ux%u back from buffer %#x, row %u\n", sFrame, w, h, fbp, y0);
    }
    if (sGpu) {
#ifdef PORT_GS_REF
        if (GsRef_Threaded()) {
            return;
        }
#endif
        {
            /* First choice: the frame stays on the card as it was drawn and the cross-fade draws from that (the
               game is handed black, which it uploads and never shows). BT3_XFADE_READ=1: the game's own way. */
            extern int GsGpu_Snapshot(uint32_t fbp);
            static unsigned snapFrame = ~0u, snapFbp = ~0u;
            if (getenv("BT3_XFADE_READ") == NULL) {
                if (snapFrame == sFrame && snapFbp == fbp) {
                    return;
                }
                if (GsGpu_Snapshot(fbp)) {
                    snapFrame = sFrame;
                    snapFbp = fbp;
                    return;
                }
            }
        }
        if (keptFrame != sFrame || keptFbp != fbp) {
            if (!GsGpu_ReadFrame(fbp, kept)) {
                return;
            }
            keptFrame = sFrame;
            keptFbp = fbp;
        }
        for (y = 0; y < h && y0 + y < 448; y++) {
            for (x = 0; x < w; x++) {
                memcpy(rgb + (y * w + x) * 3, kept + ((y0 + y) * 512 + x) * 4, 3);
            }
        }
    }
#ifdef PORT_GS_REF
    else if (Gs_Mode() != NULL) {
        GsRef_StoreImage(fbp, y0, w, h, rgb);
    }
#endif
}

/* 1: what the game hands over as records instead of GS packets is drawn (port/src/gs_marker.c Port_Quad); 0: the
   software renderer, which draws only packets (BT3_RND=0: the packets on the GPU too, to compare). */
int Gs_TakesRecords(void) {
    static int off = -1;
    if (off < 0) { off = getenv("BT3_RND") != NULL && atoi(getenv("BT3_RND")) == 0; }
    return sGpu && !off;
}

/* ---- the renderer's command queue (include/port/rnd.h) ----
   What the game hands over as records does not travel in its display list: it is queued here with the place in the
   list it belongs at (the list's first address, and the address the list's cursor had: the DMA tag written there
   next). The list is walked (gs_core.c run_chain); before the tag at an address the records queued at it are drawn,
   so they come between the packets the game put in front of them and after them. A list's records go to the walk
   when the list is handed over (Gs_RndTake); records of a list not handed over yet stay. Two queues: the one the game
   fills, the one a walk (on the render thread, BT3_GS_THREAD) reads. */
typedef struct RndCmd {
    uint32_t list, at; /* the display list's first address; the address in it */
    uint32_t kind;     /* 0 RndQuad, 1 RndPoly */
    union { RndQuad q; RndPoly p; } r;
} RndCmd;
typedef struct RndQueue {
    RndCmd *cmd;
    uint32_t n, cap, next; /* (next: the walk's) */
} RndQueue;
static RndQueue sRndFill, sRndWalk;
static unsigned sRndLost, sRndLate; /* records the queue had no memory for; records whose address the walk did not meet */

/* port/src/gs_marker.c Port_Quad, Port_Poly: record rec of kind (0 RndQuad, 1 RndPoly) at address `at` of the list
   that starts at `list`. 0 = not taken. */
int Gs_RecordPush(const void *rec, int kind, uint32_t list, uint32_t at) {
    RndQueue *f = &sRndFill;
    if (f->n != 0 && f->cmd[f->n - 1].list == list && at < f->cmd[f->n - 1].at) {
        /* the list was started again without being handed over (src/sys/dma.c Dma_ResetBuffers): its records go */
        uint32_t i, keep = 0;
        for (i = 0; i < f->n; i++) {
            if (f->cmd[i].list != list) { f->cmd[keep++] = f->cmd[i]; }
        }
        f->n = keep;
    }
    if (f->n == f->cap) {
        uint32_t cap = f->cap ? f->cap * 2 : 1024;
        RndCmd *c = cap <= 65536 ? realloc(f->cmd, cap * sizeof(RndCmd)) : NULL;
        if (c == NULL) { sRndLost++; return 0; }
        f->cmd = c;
        f->cap = cap;
    }
    f->cmd[f->n].list = list;
    f->cmd[f->n].at = at;
    f->cmd[f->n].kind = (uint32_t)kind;
    if (kind == 0) { f->cmd[f->n].r.q = *(const RndQuad *)rec; } else { f->cmd[f->n].r.p = *(const RndPoly *)rec; }
    f->n++;
    return 1;
}

/* The list at tadr is handed over: its records from the queue being filled to the one the walk reads, in order. */
void Gs_RndTake(uint32_t tadr) {
    RndQueue *f = &sRndFill, *w = &sRndWalk;
    uint32_t i, keep = 0;
    w->n = w->next = 0;
    if (w->cap < f->n) {
        RndCmd *c = realloc(w->cmd, f->n * sizeof(RndCmd));
        if (c == NULL) { sRndLost += f->n; f->n = 0; return; }
        w->cmd = c;
        w->cap = f->n;
    }
    for (i = 0; i < f->n; i++) {
        if (f->cmd[i].list == tadr) {
            w->cmd[w->n++] = f->cmd[i];
        } else {
            f->cmd[keep++] = f->cmd[i];
        }
    }
    f->n = keep;
}

static void rnd_draw(const RndCmd *c) {
    if (c->kind == 0) { GsGpu_Quad(&c->r.q); } else { GsGpu_Poly(&c->r.p); }
}

/* The walk is at the DMA tag at tadr: the records queued in front of it. */
void Gs_RndDrain(uint32_t tadr) {
    RndQueue *w = &sRndWalk;
    while (w->next < w->n && w->cmd[w->next].at == tadr) {
        rnd_draw(&w->cmd[w->next++]);
    }
}

/* The walk is done: what it did not meet (the game moved its cursor back over them) is drawn last, and counted. */
void Gs_RndRest(void) {
    RndQueue *w = &sRndWalk;
    while (w->next < w->n) {
        rnd_draw(&w->cmd[w->next++]);
        sRndLate++;
    }
    w->n = w->next = 0;
    if ((sRndLate != 0 || sRndLost != 0) && getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "rnd: frame %u: %u records not met by the walk (drawn last), %u not queued (no memory)\n", sFrame, sRndLate, sRndLost);
    }
    sRndLate = sRndLost = 0;
}

/* ---- the frame's time ---- */

uint64_t Gs_NowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The frame's work ends here (its list handed over at tChain): timed (BT3_GS_VERBOSE: per frame over the budget, per
   60 frames where the time went), and BT3_BURN_MS (testing) makes the machine slower. */
void Gs_FramePace(uint64_t tChain) {
    static uint64_t start, sum, worst, sumGame, sumList, sumTex, sumPipe, sumEnd, perMin = ~0ull, perMax;
    static unsigned n, over;
    uint64_t t;

    t = Gs_NowNs();
    if (start != 0) {
        extern unsigned long long gPortSleptNs;
        uint64_t waited = (uint64_t)gPortSleptNs;
        uint64_t work = t - start > waited ? t - start - waited : 0; /* without the waiting for the vertical blank */
        gPortSleptNs = 0;
        {   /* a frame over budget: where the time went and what was created in it */
            extern unsigned gGpuNewTex, gGpuNewTexPixels, gGpuNewPipes;
            extern uint64_t gGpuTexNs, gGpuPipeNs, gGpuEndNs;
            if (work > 33366700ull && getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "slow: frame %u: %.1f ms = game %.1f ms + display list %.1f ms; %u new textures (%u pixels), %u new pipelines\n",
                        sFrame, (double)work / 1e6, (double)(tChain - start) / 1e6, (double)(t - tChain) / 1e6, gGpuNewTex, gGpuNewTexPixels,
                        gGpuNewPipes);
                fprintf(stderr, "slow:   of the display list time: texture decoding %.1f ms, pipeline creation %.1f ms, submitting the frame %.1f ms\n",
                        (double)gGpuTexNs / 1e6, (double)gGpuPipeNs / 1e6, (double)gGpuEndNs / 1e6);
            }
            /* per second: where the work goes (the game's own code, walking the list, and of that the GPU side) */
            sumGame += tChain - start > waited ? tChain - start - waited : 0;
            sumList += t - tChain;
            sumTex += gGpuTexNs;
            sumPipe += gGpuPipeNs;
            sumEnd += gGpuEndNs;
            gGpuNewTex = gGpuNewTexPixels = gGpuNewPipes = 0;
            gGpuTexNs = gGpuPipeNs = gGpuEndNs = 0;
        }
        if (getenv("BT3_BURN_MS") != NULL) { /* testing: a slower machine, by burning time in every frame */
            uint64_t until = Gs_NowNs() + (uint64_t)atoi(getenv("BT3_BURN_MS")) * 1000000ull;
            while (Gs_NowNs() < until) {
            }
        }
        sum += work;
        if (work > worst) { worst = work; }
        if (work > 33366700ull) { over++; }
        if (++n == 60) {
            if (getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "time: frame %u: work per frame: average %.1f ms, worst %.1f ms, %u of 60 over the 33.4 ms budget\n",
                        sFrame, (double)sum / 60e6, (double)worst / 1e6, over);
                fprintf(stderr, "time:   of the average: game code %.1f ms, display list %.1f ms (of which: texture decoding %.1f, "
                                "pipeline creation %.1f, ending and submitting the frame %.1f)\n",
                        (double)sumGame / 60e6, (double)sumList / 60e6, (double)sumTex / 60e6, (double)sumPipe / 60e6, (double)sumEnd / 60e6);
            }
            if (getenv("BT3_GS_VERBOSE") != NULL) {
                fprintf(stderr, "time:   frames lasted %.1f to %.1f ms (33.4 in a fight, 16.7 in the menus)\n", (double)perMin / 1e6, (double)perMax / 1e6);
            }
            n = 0; sum = 0; worst = 0; over = 0;
            sumGame = sumList = sumTex = sumPipe = sumEnd = 0;
            perMin = ~0ull;
            perMax = 0;
        }
    }
    /* pacing is done at the vertical blank (Port_VBlank in plat_stub.c): 60 per second, the game waits for one
       per menu frame and two per battle frame */
    t = Gs_NowNs();
    if (start != 0) { /* the frame from start to start, waiting included */
        if (t - start < perMin) { perMin = t - start; }
        if (t - start > perMax) { perMax = t - start; }
    }
    start = t;
}

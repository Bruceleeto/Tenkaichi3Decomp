/*
 * The renderer's front end without the GS: what both builds need whether or not the PS2 reference (gs_core.c: GS
 * memory, the packets, the software rasteriser; gs_vu1.c) is linked. The PC build links the reference too
 * (PORT_GS_REF, from its Makefile) and hands it what only it can do; the Dreamcast build has this file alone.
 *   - which renderer draws, and starting it;
 *   - the count of pictures (gGsFrame);
 *   - the settings read from the environment (Port_GetEnv);
 *   - reading the shown picture back for the game (Gs_StoreImage);
 *   - the queue of the records the game hands over (include/port/rnd.h), drawn at their places in the list;
 *   - the frame's time;
 *   - the walk of the frame's list without the GS (the Dreamcast's; the PC's with BT3_WALK=1), and the transfers on
 *     the GIF channel the same way;
 *   - the three calls of Sony's graphics library that put something into GS memory (sceGsSetDefDBuff and the two
 *     that send what it builds), and the frame buffer the game shows (gGsMainFbp).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include "gs_internal.h"
#include "../port_addr.h"
#include "../../../include/port/rnd.h" /* the records of the queue */

unsigned gGsFrame; /* pictures so far (a frame that is only being re-run is not one) */
#define sFrame gGsFrame
static int sGpu;   /* 1: the GPU back end draws (BT3_GS=gpu) */

static int sWalk;  /* the lists are walked here, not by the reference (the Dreamcast; BT3_WALK=1..3 on PC): 1 the markers
                      carried out as the walk meets them; 2 the markers queued with the records (Port_Mark), the walk
                      only counting them; 3 the same and the frame's list not walked: the queue drawn in order */
int Gs_MarksQueued(void) { return sWalk >= 2 && sGpu; }
/* 1: something reads the frame's display list for what the PS2 alone needs from it (VU1's microprograms and
   constants, ...): the PS2 reference. 0: nothing does (the Dreamcast; BT3_WALK=3 on PC): such packets are not
   built (src/sys/vu1_packet.c). */
int Port_ListRead(void) {
#ifdef PORT_GS_REF
    return sWalk != 3;
#else
    return 0;
#endif
}

#ifdef PORT_GS_REF
int GsRef_Start(int gpu);  /* gs_core.c: the reference's tables and switches, the GPU back end (on its thread) */
int GsRef_Threaded(void);  /* gs_core.c: 1 = the back end runs on the render thread (BT3_GS_THREAD) */
void GsRef_StoreImage(unsigned fbp, unsigned y0, unsigned w, unsigned h, unsigned char *rgb); /* from GS memory */
void GsRef_Vif1Chain(uint32_t tadr, int tte);              /* gs_core.c: the list, by the reference */
void GsRef_GifChannel(uint32_t addr, uint32_t qwc, int chain); /* gs_core.c: a GIF transfer, by the reference */
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
#ifndef PORT_GS_REF
        (void)gPortDataStripped;
        return "gpu"; /* (the Dreamcast: no environment, always the PVR) */
#else
        return gPortDataStripped == 1 ? "gpu" : NULL;
#endif
    }
    return strcmp(m, "none") == 0 || strcmp(m, "off") == 0 || strcmp(m, "0") == 0 ? NULL : m;
}

/* 1 once a renderer is chosen (the first transfer to the GS starts it); 0: nothing is drawn. With the reference
   linked, BT3_WALK=1 has the lists walked here as on the Dreamcast (the GPU back end on this thread; the reference
   gets its tables but draws nothing), so that what still goes by packet can be seen against the reference. */
int Gs_On(void) {
    static int on = -1;

    if (on < 0) {
        int gpu;
        on = Gs_Mode() != NULL;
        gpu = on && strcmp(Gs_Mode(), "gpu") == 0;
#ifdef PORT_GS_REF
        sWalk = getenv("BT3_WALK") != NULL ? atoi(getenv("BT3_WALK")) : 0;
        sWalk = sWalk < 0 ? 0 : sWalk > 3 ? 3 : sWalk;
        sGpu = GsRef_Start(gpu && !sWalk);
        if (sWalk) {
            sGpu = gpu && GsGpu_Init();
        }
#else
        sWalk = 3; /* (the frame's list not walked: the queue drawn in order) */
        sGpu = gpu && GsGpu_Init();
#endif
    }
    return on;
}

int GsGpu_Enabled(void) {
    return sGpu;
}

/* The picture is finished: counted, and BT3_XFADE_AT=<frame> (testing) asks for the fight's screen cross-fade at
   that frame, as the game does between the shots of an intro, so a run without a window can show it in any fight.
   BT3_GS_FROM=<frame> (testing): nothing is drawn before that frame, the frames are left out as frame skipping
   leaves them out (the software reference is slow, and so is the PVR's on PC): a run reaches a late frame sooner. */
void Gs_FrameCount(void) {
    static int at = -2, from = -2;
#ifdef PORT_GS_REF
    Rnd_GsFollowFrame();
#endif
    if (at == -2) {
        at = getenv("BT3_XFADE_AT") != NULL ? atoi(getenv("BT3_XFADE_AT")) : -1;
        from = getenv("BT3_GS_FROM") != NULL ? atoi(getenv("BT3_GS_FROM")) : 0;
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
    if ((int)sFrame < from) {
        gsSkipFrame = 1; /* (for the frame that comes now: the renderer's end of the last one decided it) */
    }
}

/* The game reads a piece of a frame buffer back (sceGsExecStoreImage; the cross-fade of the fight's scenes takes the
   frame being shown, 512 x 224 at a time). `bp` in units of 64 pixels: 0xE00 a frame buffer (32 a page), 8 a row of
   512. Delivered as 3 bytes a pixel, top row first.
     - software renderer: from its GS memory (gs_core.c);
     - GPU back end: the frame buffer's picture is read from the card once and kept for the calls that follow in the
       same frame (two halves), reduced to the GS's 512 x 448;
     - nothing drawn (tests) or the back end on a thread of its own: black, and the game is not told otherwise;
     - BT3_NO_RTT (the Dreamcast's build without DC_RTT=1): black, as frame buffers are not read at all. */
void Gs_StoreImage(unsigned bp, unsigned w, unsigned h, unsigned char *rgb) {
#ifndef BT3_NO_RTT
    extern int GsGpu_ReadFrame(uint32_t fbp, uint8_t *rgba); /* gs_draw.c */
    static uint8_t kept[512 * 448 * 4];
    static unsigned keptFrame = ~0u, keptFbp = ~0u;
#endif
    unsigned fbp = bp / 0xE00 * 0x70, y0 = bp % 0xE00 / 8, x, y;

    if (w > 512) { w = 512; }
    memset(rgb, 0, (size_t)w * h * 3);
    if (getenv("BT3_GS_VERBOSE") != NULL || getenv("BT3_STORE_LOG") != NULL) {
        fprintf(stderr, "gs: frame %u: the game reads %ux%u back from buffer %#x, row %u\n", sFrame, w, h, fbp, y0);
    }
#ifndef BT3_NO_RTT
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
   list it belongs at (the address the list's cursor had: the DMA tag written there next). The list is walked (the
   reference, gs_core.c run_chain); before the tag at an address the records queued at it are drawn, so they come
   between the packets the game put in front of them and after them. A transfer is a call (sys/dma.c), so what is
   queued is always the list being made: all of it goes to the walk when the list is handed over (Gs_RndTake), and
   it is dropped when the game starts the list again without sending it (Dma_ResetBuffers: Port_ListRestart). Two
   queues: the one the game fills, the one a walk (on the render thread, BT3_GS_THREAD) reads.
   Each entry is its place, its kind and its record, kept at its own size in the queue's chunks (an entry that held
   the largest was 168 bytes, a frame's some MB on the Dreamcast with a model's every triangle): blocks of 32 KB
   taken as the list grows and kept for the next lists, so that a record does not move once it is queued and the
   queue is not copied to grow (two blocks at once, the old and the new).
   A draw made later (Gs_DeferPush: a model's mesh, port/src/vu1_native.c) is queued as the function that makes it
   and what it needs to be made, called in its place when the queue is drawn: its records drawn as they come
   (sReplay), none kept. */
typedef void (*RndDeferFn)(const void *);
typedef struct RndCmd {
    uint32_t at;      /* the address in the display list */
    uint32_t kind;    /* 0 RndQuad, 1 RndPoly, 2 a marker (register, value), 3 a draw made later (its RndDeferFn,
                         then its block) */
    const uint8_t *d; /* its record */
} RndCmd;
#define CHUNK_DATA (32 * 1024 - 16)
typedef struct RndChunk {
    struct RndChunk *next;
    uint32_t used;
    uint64_t data[CHUNK_DATA / 8]; /* (8-aligned) */
} RndChunk;
typedef struct RndQueue {
    RndCmd *cmd;
    uint32_t n, cap, next; /* (next: the walk's) */
    RndChunk *head, *cur;  /* its chunks; the one being filled (NULL: none yet in this list) */
    uint32_t gen;          /* (the fill queue's: which list it holds, Gs_DeferGen) */
} RndQueue;
static RndQueue sRndFill, sRndWalk;
static unsigned sMarksIn; /* markers queued (Port_Mark) since the last report */
static unsigned sRndLost, sRndLate; /* records the queue had no memory for; records whose address the walk did not meet */
static int sReplay; /* a draw made later is being made: its records go straight to the renderer */

/* src/sys/dma.c Dma_ResetBuffers: the list is started again without being handed over: its records go. */
void Port_BindUndo(void); /* port/src/gs_marker.c */
void Port_BindKeep(void);
void Port_ListRestart(void) {
    sRndFill.n = 0;
    sRndFill.cur = NULL;
    sRndFill.gen++;
    Port_BindUndo();
}

/* An entry of kind at `at` with `bytes` of the queue's chunks for its record (8-aligned); NULL: no memory (said). */
static void *rnd_entry(int kind, uint32_t at, uint32_t bytes, const void *from) {
    RndQueue *f = &sRndFill;
    RndChunk *c = f->cur;
    uint32_t ofs;
    if (bytes > CHUNK_DATA) {
        sRndLost++;
        Port_Lost("a record larger than a chunk of the queue (its bytes)", bytes, from);
        return NULL;
    }
    if (f->n == f->cap) {
        uint32_t cap = f->cap ? f->cap * 2 : 1024;
        RndCmd *n = cap <= 1 << 20 ? realloc(f->cmd, cap * sizeof(RndCmd)) : NULL;
        if (n == NULL) {
            sRndLost++;
            Port_Lost("a record: the frame's queue is full (its records)", f->n, from);
            return NULL;
        }
        f->cmd = n;
        f->cap = cap;
    }
    if (c == NULL || ((c->used + 7) & ~7u) + bytes > CHUNK_DATA) { /* the next chunk: one kept, or a new one */
        RndChunk *n = c != NULL ? c->next : f->head;
        if (n == NULL) {
            if ((n = malloc(sizeof(RndChunk))) == NULL) {
                sRndLost++;
                Port_Lost("a record: no memory for another chunk of the queue (its records)", f->n, from);
                return NULL;
            }
            n->next = NULL;
            if (c != NULL) { c->next = n; } else { f->head = n; }
        }
        n->used = 0;
        f->cur = c = n;
    }
    ofs = (c->used + 7) & ~7u;
    c->used = ofs + bytes;
    f->cmd[f->n].at = at;
    f->cmd[f->n].kind = (uint32_t)kind;
    f->cmd[f->n].d = (const uint8_t *)c->data + ofs;
    f->n++;
    return (uint8_t *)c->data + ofs;
}

/* port/src/gs_marker.c Port_Quad, Port_Poly, Port_Mark: record rec of kind (0 RndQuad, 1 RndPoly, 2 a marker) at
   address `at` of the list being made. 0 = not taken. */
int Gs_RecordPush(const void *rec, int kind, uint32_t at) {
    uint32_t bytes = kind == 0 ? sizeof(RndQuad) : kind == 1 ? sizeof(RndPoly) : 16;
    void *d;
    if (sReplay) {
        if (kind == 0) { GsGpu_Quad((const RndQuad *)rec); } else if (kind == 1) { GsGpu_Poly((const RndPoly *)rec); }
        else { Gs_Marker((uint32_t)((const uint64_t *)rec)[0], ((const uint64_t *)rec)[1]); }
        return 1;
    }
    if ((d = rnd_entry(kind, at, bytes, __builtin_return_address(0))) == NULL) {
        return 0;
    }
    memcpy(d, rec, bytes);
    sMarksIn += kind == 2;
    return 1;
}

/* Whether a draw can be made later (Gs_DeferPush): where the queue is drawn in the call that hands the list over
   (BT3_WALK=3, the Dreamcast), so the state a draw is made with is set again (by its caller) on the same thread. */
int Gs_Defers(void) {
    return sWalk == 3 && Gs_TakesRecords() && !sReplay;
}

/* A draw made later, at the address `at` of the list being made: `bytes` of the queue's chunks for what it needs
   (filled by the caller; 8-aligned; there until the list is drawn), fn called with them in its place when the
   queue is drawn. NULL: no memory (said). */
void *Gs_DeferPush(uint32_t bytes, RndDeferFn fn, uint32_t at) {
    uint8_t *d = rnd_entry(3, at, 8 + bytes, __builtin_return_address(0));
    if (d == NULL) {
        return NULL;
    }
    memcpy(d, &fn, sizeof(fn));
    return d + 8;
}

/* Which list the queue being filled holds: another once it is handed over or started again (a list's blocks are
   gone once it is drawn or started again: a block names only one of its own list). */
uint32_t Gs_DeferGen(void) {
    return sRndFill.gen;
}

/* The list at tadr is handed over: the queue being filled becomes the one the walk reads, as it is (its block
   changes hands; nothing is copied), and the walk's block, read to the end, is the next to be filled. */
void Gs_RndTake(void) {
    RndQueue *f = &sRndFill, *w = &sRndWalk, t;
    Port_BindKeep();
    t = *w;
    *w = *f;
    w->next = 0;
    *f = t;
    f->n = f->next = 0;
    f->cur = NULL;
    f->gen = w->gen + 1;
}

/* The walk has drawn the list, in the same call that handed it over (the Dreamcast; the PC's but for
   gs_core.c's render thread): its blocks go back to the queue to be filled, so that there is one of each and not
   two. */
static void rnd_walked(void) {
    RndQueue *f = &sRndFill, *w = &sRndWalk;
    if (f->n == 0 && w->cap > f->cap) {
        free(f->cmd);
        f->cmd = w->cmd;
        f->cap = w->cap;
        w->cmd = NULL;
        w->cap = 0;
    }
    if (f->n == 0 && f->head == NULL) { /* (the chunks to the queue to be filled; where it has its own, freed) */
        f->head = w->head;
    } else {
        while (w->head != NULL) {
            RndChunk *c = w->head;
            w->head = c->next;
            free(c);
        }
    }
    w->head = w->cur = NULL;
    w->n = w->next = 0;
}

static uint64_t sMeshNs; /* this frame's time making the queued meshes (Gs_FramePace) */

static void rnd_draw(const RndCmd *c) {
    const uint8_t *d = c->d;
    if (c->kind == 0) { GsGpu_Quad((const RndQuad *)d); } else if (c->kind == 1) { GsGpu_Poly((const RndPoly *)d); }
    else if (c->kind == 3) {
        RndDeferFn fn;
        uint64_t t0 = Gs_NowNs();
        memcpy(&fn, d, sizeof(fn));
        sReplay = 1;
        fn(d + 8);
        sReplay = 0;
        sMeshNs += Gs_NowNs() - t0;
    } else { Gs_Marker((uint32_t)((const uint64_t *)d)[0], ((const uint64_t *)d)[1]); }
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
    w->cur = NULL;
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
/* the time report every 60 frames: BT3_GS_VERBOSE or BT3_FRAME_TIME; always on the Dreamcast (no environment) */
static int time_report(void) {
#ifdef __DREAMCAST__
    return 1;
#else
    static int on = -1;
    if (on < 0) { on = getenv("BT3_GS_VERBOSE") != NULL || getenv("BT3_FRAME_TIME") != NULL; }
    return on;
#endif
}

void Gs_FramePace(uint64_t tChain) {
    static uint64_t start, sum, worst, sumGame, sumList, sumTex, sumPipe, sumEnd, sumMesh, perMin = ~0ull, perMax;
    static unsigned n, over;
    static uint64_t fpsFrom;
    static unsigned fpsN;
    uint64_t t;

    t = Gs_NowNs();
    if (fpsFrom == 0) { fpsFrom = t; }
    if (++fpsN, t - fpsFrom >= 5000000000ull) { /* frames a second, every 5 seconds */
        printf("fps: %.1f\n", (double)fpsN * 1e9 / (double)(t - fpsFrom));
        fflush(stdout);
        fpsFrom = t;
        fpsN = 0;
    }
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
            sumMesh += sMeshNs;
            gGpuNewTex = gGpuNewTexPixels = gGpuNewPipes = 0;
            gGpuTexNs = gGpuPipeNs = gGpuEndNs = 0;
            sMeshNs = 0;
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
            if (time_report()) {
                fprintf(stderr, "time: frame %u: work per frame: average %.1f ms, worst %.1f ms, %u of 60 over the 33.4 ms budget\n",
                        sFrame, (double)sum / 60e6, (double)worst / 1e6, over);
                fprintf(stderr, "time:   of the average: game code %.1f ms, display list %.1f ms (of which: the models' meshes %.1f, texture "
                                "decoding %.1f, pipeline creation %.1f, ending and submitting the frame %.1f)\n",
                        (double)sumGame / 60e6, (double)sumList / 60e6, (double)sumMesh / 60e6, (double)sumTex / 60e6, (double)sumPipe / 60e6,
                        (double)sumEnd / 60e6);
            }
            if (time_report()) {
                fprintf(stderr, "time:   frames lasted %.1f to %.1f ms (33.4 in a fight, 16.7 in the menus)\n", (double)perMin / 1e6, (double)perMax / 1e6);
            }
            n = 0; sum = 0; worst = 0; over = 0;
            sumGame = sumList = sumTex = sumPipe = sumEnd = sumMesh = 0;
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

/* ---- the port's registers ----
   GS registers 0x7B..0x7F, which the real GS does not have: what the list says to the PVR layer in place of what it
   no longer sends (gs_pvr.c) and where a native effect goes (gs_marker.c). Written by the walk, or by the reference's
   reg_write when it draws. */
uint32_t gGsPageGen[512];         /* per page: bumped whenever an upload writes into that 8 KB page of GS memory (by the
                                     reference; by the markers below) */
uint64_t gGsTint[512];            /* per page: the tint of the palettes there this frame (gs_pvr.c Port_ClutTint), cleared
                                     each frame */
static uint64_t sTintNext;        /* the tint the next 0x7C sets */

/* A marker of gs_pvr.c in place of an upload (0x7D, 0x7E): d = block | blocks << 48. The pages the upload would
   have written changed, as with an upload (what was drawn there is no longer there). (What it bound there the
   records were given when they were made: gs_marker.c Port_Bind.) */
static void nat_mark(uint64_t d) {
    uint32_t b = (uint32_t)d & 0x3FFF, n = (uint32_t)(d >> 48), i;
    for (i = b / 32; i <= (b + (n ? n : 1) - 1) / 32; i++) {
        gGsPageGen[i & 511]++;
    }
}

void Gs_Marker(uint32_t addr, uint64_t d) {
    switch (addr) {
    case 0x7F: if (sGpu) { GsGpu_Native((int)d); } break; /* a native effect goes here (gs_marker.c) */
    case 0x7B: sTintNext = d; break; /* (a tint of the palettes of the two pages from page d: gs_pvr.c Port_ClutTint) */
    case 0x7C: gGsTint[d & 511] = gGsTint[(d + 1) & 511] = sTintNext; break;
    case 0x7D: nat_mark(d); break; /* a palette of tex.pak in place of its upload: gs_pvr.c Port_PalNative */
    case 0x7E: nat_mark(d); break; /* a texture entry as a PVR texture of its own in place of its pixels: Port_TexNative */
    default: break;
    }
}

/* ---- the GS state ----
   What the packets set in the GS, which gs_draw.c still reads for what is drawn by packet and to check the records
   against: the reference's to keep (gs_core.c reg_write). Without it (the Dreamcast) nothing sets it and there is
   no GS memory: it reads as it does on the PC when the lists are walked, never written, and empty. */
GsState gGs;

int Gs_PsmBits(uint32_t psm) {
    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F; /* a depth buffer read as a texture: 32, 24 or 16 bits like the colour formats */
    }
    switch (psm) {
    case 0x00: return 32;
    case 0x01: return 24;
    case 0x02: case 0x0A: return 16;
    case 0x13: case 0x1B: return 8;
    case 0x14: case 0x24: case 0x2C: return 4;
    default: return 32;
    }
}

/* A stored colour of format psm as R, G, B, A (TEXA supplies alpha where the format has none or one bit). */
uint32_t Gs_Expand(uint32_t c, uint32_t psm) {
    uint32_t ta0 = gGs.texa & 0xFF, ta1 = (gGs.texa >> 32) & 0xFF, aem = (gGs.texa >> 15) & 1;

    if ((psm & 0x30) == 0x30) {
        psm &= 0x0F;
    }

    if (psm == 0x00) {
        return c;
    }
    if (psm == 0x01) {
        c &= 0xFFFFFF;
        return c | ((aem && c == 0 ? 0 : ta0) << 24);
    }
    {
        uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
        uint32_t a = (c & 0x8000) ? ta1 : (aem && (c & 0x7FFF) == 0 ? 0 : ta0);
        return r | g << 8 | b << 16 | a << 24;
    }
}

/* ---- the walk ----
   The frame's list as the Dreamcast takes it (and the PC with BT3_WALK=1, in place of the reference's run_chain): the
   DMA chain is followed only to find where the queued records go, and the port's markers in its GIF packets (GS
   registers 0x7B..0x7F, which the real GS does not have) are carried out. Everything else the list still holds for
   the GS is passed over and counted (BT3_GS_VERBOSE): what it would have uploaded, drawn, set, or run on VU1. Of each
   layer only as much is read as it takes to find the next: the DMA tags, the VIF codes (with STCYCL, which an
   UNPACK's length depends on), the GIF tags and the register numbers of their A+D writes. */
enum { W_UPLOAD, W_IMAGE_QW, W_DRAW, W_VERTS, W_STATE, W_VU1_CALL, W_VU1_QW, W_MPG, W_MARK, W_COUNT };
static unsigned sW[2][W_COUNT]; /* passed over: [0] in the frame's list, [1] on the GIF channel since the last list */
static int sWSrc;               /* what is being walked: 0 the list, 1 the GIF channel */
static uint32_t sWTag;          /* the DMA tag whose data is being walked (BT3_WALK_LOG) */

/* BT3_WALK_LOG=<frame>: each GIF packet passed over in that frame, with the address of the DMA tag it came with. */
static int walk_log(void) {
    static int at = -2;
    if (at == -2) {
        at = getenv("BT3_WALK_LOG") != NULL ? atoi(getenv("BT3_WALK_LOG")) : -1;
    }
    return at >= 0 && (int)sFrame == at;
}

/* A write to one of the port's registers, met by the walk. */
static void walk_mark(uint32_t addr, uint64_t d) {
    sW[sWSrc][W_MARK]++;
    if (sWSrc == 1 || !Gs_MarksQueued()) { Gs_Marker(addr, d); } /* (else the list's are in the queue) */
}

static int is_vertex(uint32_t r) {
    return r == 0x4 || r == 0x5 || r == 0xC || r == 0xD;
}

/* The GIF tag being read (walk_gif, read from game address at), counted as kind (W_UPLOAD, W_DRAW, W_STATE). */
static void tag_count(int kind, const uint64_t *tag, uint32_t at) {
    sW[sWSrc][kind]++;
    if (walk_log()) {
        uint32_t flg = (uint32_t)(tag[0] >> 58) & 3, nreg = (uint32_t)(tag[0] >> 60) & 15;
        fprintf(stderr, "walk: frame %u: %s, DMA tag at %08x: GIF tag at %08x: %s, %u loops, flg %u, %u registers %016llx%s\n", sFrame,
                sWSrc ? "GIF channel" : "list", (unsigned)sWTag, (unsigned)at, kind == W_UPLOAD ? "upload" : kind == W_DRAW ? "draw" : "state",
                (unsigned)(tag[0] & 0x7FFF), (unsigned)flg, (unsigned)(nreg ? nreg : 16), (unsigned long long)tag[1], ((tag[0] >> 46) & 1) ? " (with PRIM)" : "");
    }
}

/* One run of GIF data (whole quadwords): the tags, and in PACKED the A+D writes, read; the rest passed over. A tag's
   data may continue in the next run, as for the GS. */
static void walk_gif(const uint8_t *p, uint32_t qwc) {
    static uint64_t tag[2];     /* current tag: persists across runs */
    static uint32_t loops, reg; /* loops left, register index within the loop */
    static int kind;            /* what it is counted as (tag_count); -1 the port's markers, -2 not known yet */
    static uint32_t at;         /* where the tag was read */
    const uint64_t *q = (const uint64_t *)p;

    while (qwc != 0) {
        uint32_t flg, nreg, i;

        if (loops == 0) {
            tag[0] = q[0];
            tag[1] = q[1];
            at = Port_Addr(q);
            q += 2;
            qwc--;
            loops = tag[0] & 0x7FFF;
            reg = 0;
            if (loops == 0) {
                continue;
            }
            flg = (tag[0] >> 58) & 3;
            nreg = (tag[0] >> 60) & 15;
            nreg = nreg ? nreg : 16;
            if (flg >= 2) {
                kind = W_UPLOAD;
            } else if (flg == 0 && nreg == 1 && (tag[1] & 15) == 0xE) {
                kind = -2; /* A+D alone: the first write says what it is (the port's markers are packets of their own) */
                continue;
            } else {
                kind = W_STATE;
                for (i = 0; i < nreg; i++) {
                    if (is_vertex((uint32_t)(tag[1] >> (4 * i)) & 15)) {
                        kind = W_DRAW;
                    }
                }
            }
            tag_count(kind, tag, at);
            continue;
        }
        flg = (tag[0] >> 58) & 3;
        nreg = (tag[0] >> 60) & 15;
        nreg = nreg ? nreg : 16;
        if (flg == 0) { /* PACKED: one quadword per register */
            uint32_t r = (tag[1] >> (4 * reg)) & 15;
            if (r == 0xE) {
                uint32_t a = (uint32_t)(q[1] & 0xFF);
                if (kind == -2) {
                    kind = a >= 0x7B && a <= 0x7F ? -1 : is_vertex(a) ? W_DRAW : W_STATE;
                    if (kind >= 0) {
                        tag_count(kind, tag, at);
                    }
                }
                if (kind >= 0 && walk_log()) {
                    fprintf(stderr, "walk:   A+D %02x %016llx\n", (unsigned)a, (unsigned long long)q[0]);
                }
                if (a >= 0x7B && a <= 0x7F) {
                    walk_mark(a, q[0]);
                } else if (is_vertex(a)) {
                    if (kind == W_STATE) { /* (a state packet that turns out to draw) */
                        sW[sWSrc][W_STATE]--;
                        sW[sWSrc][W_DRAW]++;
                        kind = W_DRAW;
                    }
                    sW[sWSrc][W_VERTS]++;
                }
            } else if (is_vertex(r)) {
                sW[sWSrc][W_VERTS]++;
            }
            q += 2;
            qwc--;
            if (++reg == nreg) {
                reg = 0;
                loops--;
            }
        } else if (flg == 1) { /* REGLIST: 64 bits per register */
            int half;
            for (half = 0; half < 2 && loops != 0; half++) {
                if (is_vertex((uint32_t)(tag[1] >> (4 * reg)) & 15)) {
                    sW[sWSrc][W_VERTS]++;
                }
                if (++reg == nreg) {
                    reg = 0;
                    loops--;
                }
            }
            q += 2;
            qwc--;
        } else { /* IMAGE */
            uint32_t n = loops < qwc ? loops : qwc;
            sW[sWSrc][W_IMAGE_QW] += n;
            q += 2 * n;
            qwc -= n;
            loops -= n;
        }
    }
}

/* The counts of the frame (BT3_GS_VERBOSE, every 30 frames): what was passed over. */
static void walk_report(void) {
    if (getenv("BT3_GS_VERBOSE") != NULL && sFrame % 30 == 0) {
        int k;
        for (k = 0; k < 2; k++) {
            const unsigned *c = sW[k];
            fprintf(stderr, "walk: frame %u: %s: passed over %u uploads (%u quadwords), %u draw packets (%u vertices), %u state packets, "
                            "%u VU1 calls (%u vectors unpacked, %u microprograms); %u markers\n", sFrame,
                    k ? "GIF channel since the last list" : "list", c[W_UPLOAD], c[W_IMAGE_QW], c[W_DRAW], c[W_VERTS], c[W_STATE],
                    c[W_VU1_CALL], c[W_VU1_QW], c[W_MPG], c[W_MARK]);
        }
        fprintf(stderr, "walk: frame %u: %u markers queued; %u textures drawn white (not in tex.pak, no GS memory)\n", sFrame, sMarksIn,
                GsDraw_TexMisses());
    }
    memset(sW, 0, sizeof(sW));
    sMarksIn = 0;
}

#ifdef PORT_GS_REF /* the frame's list walked (BT3_WALK=1, 2): the PC only, to compare */
/* One run of VIF data (32-bit words): DIRECT's GIF data to walk_gif, the data of every other command passed over.
   A command's data may continue in the next run (the command word often sits in the DMA tag, its data behind). */
static void walk_vif(const uint32_t *w, uint32_t count) {
    static uint32_t direct;  /* quadwords of GIF data still to come */
    static uint32_t skip;    /* words of a command's data still to come */
    static uint32_t cl, wl;  /* STCYCL */
    uint32_t i = 0;

    while (i < count) {
        uint32_t code, cmd, num, imm;

        if (direct != 0) {
            uint32_t qwc = (count - i) / 4 < direct ? (count - i) / 4 : direct;
            if (qwc == 0) {
                break; /* misaligned run: dropped, as by the reference */
            }
            walk_gif((const uint8_t *)&w[i], qwc);
            i += qwc * 4;
            direct -= qwc;
            continue;
        }
        if (skip != 0) {
            uint32_t n = count - i < skip ? count - i : skip;
            i += n;
            skip -= n;
            continue;
        }
        code = w[i++];
        cmd = (code >> 24) & 0x7F;
        num = (code >> 16) & 0xFF;
        imm = code & 0xFFFF;
        switch (cmd) {
        case 0x01: cl = imm & 0xFF; wl = (imm >> 8) & 0xFF; break;
        case 0x14: case 0x15: case 0x17: sW[0][W_VU1_CALL]++; break; /* MSCAL, MSCALF, MSCNT */
        case 0x20: skip = 1; break;                                   /* STMASK */
        case 0x30: case 0x31: skip = 4; break;                        /* STROW, STCOL */
        case 0x4A: skip = (num ? num : 256) * 2; sW[0][W_MPG]++; break; /* MPG: a microprogram */
        case 0x50: case 0x51: direct = imm ? imm : 65536; break;      /* DIRECT, DIRECTHL */
        default:
            if (cmd >= 0x60) { /* UNPACK: as many words as the VIF reads for it (gs_vu1.c GsVu1_UnpackWords) */
                uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
                uint32_t bits = vn == 3 && vl == 3 ? 16 : (32u >> vl) * (vn + 1);
                if (wl > cl && wl != 0) {
                    n = cl * (n / wl) + (n % wl > cl ? cl : n % wl);
                }
                skip = (n * bits + 31) / 32;
                sW[0][W_VU1_QW] += n;
            }
            break;
        }
    }
}

/* One frame's list: the DMA chain at `tadr` on VIF1, the records at their places, then the end of the frame. */
static void walk_list(uint32_t tadr, int tte) {
    uint32_t stack[2], sp = 0;
    int guard;

    sWSrc = 0;
    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)Port_Ptr(tadr);
        uint32_t qwc = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, addr = tag[1]; /* a game address: all 32 bits */
        const uint32_t *data;
        int end = 0;

        Gs_RndDrain(tadr);
        sWTag = tadr;
        switch (id) {
        case 0: data = (const uint32_t *)Port_Ptr(addr); end = 1; break;                 /* REFE */
        case 1: data = tag + 4; tadr += 16 + qwc * 16; break;                             /* CNT */
        case 2: data = tag + 4; tadr = addr; break;                                       /* NEXT */
        case 3: case 4: data = (const uint32_t *)Port_Ptr(addr); tadr += 16; break;      /* REF, REFS */
        case 5: data = tag + 4; if (sp < 2) { stack[sp++] = tadr + 16 + qwc * 16; } tadr = addr; break; /* CALL */
        case 6: data = tag + 4; if (sp > 0) { tadr = stack[--sp]; } else { end = 1; } break;          /* RET */
        default: data = tag + 4; end = 1; break;                                          /* END */
        }
        if (tte) {
            walk_vif(tag + 2, 2);
        }
        walk_vif(data, qwc * 4);
        if (end) {
            break;
        }
    }
}

static void walk_chain(uint32_t tadr, int tte) {
    walk_list(tadr, tte);
    Gs_RndRest();
    rnd_walked();
    walk_report();
    if (sGpu) {
        GsGpu_FrameEnd();
    }
    Gs_FrameCount();
}
#endif

/* One frame's list not walked (sWalk 3): its records and markers in the order they were queued, then the end of
   the frame. (On PC with BT3_GS_VERBOSE the list is still walked afterwards, only to count what it holds that
   nothing reads: walk_report.) */
static void list_draw(uint32_t tadr, int tte) {
    RndQueue *w = &sRndWalk;
    while (w->next < w->n) {
        rnd_draw(&w->cmd[w->next++]);
    }
    rnd_walked();
#ifdef PORT_GS_REF
    if (getenv("BT3_GS_VERBOSE") != NULL) {
        walk_list(tadr, tte);
    }
#else
    (void)tadr; (void)tte;
#endif
    if (sRndLost != 0 && getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "rnd: frame %u: %u records not queued (no memory)\n", sFrame, sRndLost);
    }
    sRndLost = 0;
    walk_report();
    if (sGpu) {
        GsGpu_FrameEnd();
    }
    Gs_FrameCount();
}

/* Carries out a source-chain transfer on VIF1 that starts at `tadr` (PS2 address = host address): the frame's list. */
void Port_GsVif1Chain(uint32_t tadr, int tte) {
    uint64_t tChain;

    if (!Gs_On()) {
        return;
    }
#ifdef PORT_GS_REF
    if (!sWalk) {
        GsRef_Vif1Chain(tadr, tte);
        return;
    }
#endif
    if (!sGpu) {
        return;
    }
    tChain = Gs_NowNs();
    Gs_RndTake();
#ifdef PORT_GS_REF
    if (sWalk != 3) {
        walk_chain(tadr, tte);
    } else
#endif
    {
        list_draw(tadr, tte);
    }
    Gs_FramePace(tChain); /* the frame's work ends here */
}

/* A transfer on the GIF channel itself (the game uses it for texture uploads outside the frame's list, the Sony
   library for the clear of each frame): `qwc` quadwords of GIF data at `addr`, or with chain = 1 a source chain of
   them starting at `addr`. */
void Port_GsGifChannel(uint32_t addr, uint32_t qwc, int chain) {
    int guard;

    if (!Gs_On()) {
        return;
    }
#ifdef PORT_GS_REF
    if (!sWalk) {
        GsRef_GifChannel(addr, qwc, chain);
        return;
    }
#endif
    if (!Port_ListRead()) { /* nothing in it is read (no markers are sent where the list is not read): only counted,
                               for BT3_GS_VERBOSE */
        static int verbose = -1;
        if (verbose < 0) {
            verbose = getenv("BT3_GS_VERBOSE") != NULL;
        }
        if (!verbose) {
            return;
        }
    }
    sWSrc = 1;
    sWTag = addr;
    if (!chain) {
        walk_gif((const uint8_t *)Port_Ptr(addr), qwc);
        sWSrc = 0;
        return;
    }
    for (guard = 0; guard < 100000; guard++) {
        const uint32_t *tag = (const uint32_t *)Port_Ptr(addr);
        uint32_t n = tag[0] & 0xFFFF, id = (tag[0] >> 28) & 7, ref = tag[1];

        sWTag = addr;
        if (id == 0 || id == 3 || id == 4) {
            walk_gif((const uint8_t *)Port_Ptr(ref), n);
            addr += 16;
        } else {
            walk_gif((const uint8_t *)(tag + 4), n);
            addr = id == 2 ? ref : addr + 16 + n * 16;
        }
        if (id == 0 || id == 7) {
            break;
        }
    }
    sWSrc = 0;
}

/* ---- the Sony layer ----
   The game leaves the per-frame CLEAR of the colour and depth buffers to the library: sceGsSetDefDBuff builds, for
   each of the two buffers, a GIF packet "drawing environment + clear sprite" inside the sceGsDBuff structure (the
   game then patches the sprite's corners and colour), and sceGsSwapDBuff sends the packet of the buffer about to be
   drawn. Without them the reference clears nothing between frames (seen as smears trailing behind camera pans). The
   layout is libgraph's (include/sys/gfx.h in the game sources documents it). When the lists are walked (the
   Dreamcast; BT3_WALK=1) nothing is sent: the PVR renderer starts with the first transfer, clears colour and depth
   at the start of each render itself (its background), and of the swap only takes which buffer is shown. */
int gGsMainFbp = -1; /* frame-buffer address of the frame being drawn (from sceGsSwapDBuff); -1 before the first swap */

typedef struct { uint64_t v, addr; } Reg;                      /* one A+D register write */
typedef struct { Reg frame, zbuf, xyoffset, scissor, prmodecont, colclamp, dthe, test; } DrawEnv;
typedef struct { Reg testa, prim, rgbaq, xyz2a, xyz2b, testb; } Clear;
typedef struct {
    uint64_t disp[2][5];
    uint64_t giftag0[2];
    DrawEnv draw0;
    Clear clear0;
    uint64_t giftag1[2];
    DrawEnv draw1;
    Clear clear1;
} DBuff;

static void set(Reg *r, uint64_t v, uint64_t addr) {
    r->v = v;
    r->addr = addr;
}

static void fill(uint64_t *tag, DrawEnv *d, Clear *c, uint32_t fbp, uint32_t zbp, int psm, int w, int h, int ztest, int zpsm, int clear) {
    uint64_t test = ztest ? (1ull << 16) | ((uint64_t)(ztest & 3) << 17) : 0;

    tag[0] = (uint64_t)(clear ? 14 : 8) | (1ull << 15) | (1ull << 60); /* NLOOP, EOP, one register per loop */
    tag[1] = 0xE;                                                      /* A+D */
    set(&d->frame, fbp | (uint64_t)((w + 63) / 64) << 16 | (uint64_t)psm << 24, 0x4C);
    set(&d->zbuf, zbp | (uint64_t)(zpsm & 15) << 24 | (uint64_t)(ztest == 0) << 32, 0x4E);
    set(&d->xyoffset, (uint64_t)((2048 - w / 2) << 4) | (uint64_t)((2048 - h / 2) << 4) << 32, 0x18);
    set(&d->scissor, (uint64_t)(w - 1) << 16 | (uint64_t)(h - 1) << 48, 0x40);
    set(&d->prmodecont, 1, 0x1A);
    set(&d->colclamp, 1, 0x46);
    set(&d->dthe, (psm & 2) ? 1 : 0, 0x45);
    set(&d->test, test, 0x47);
    set(&c->testa, (1ull << 16) | (1ull << 17), 0x47); /* depth test ALWAYS: the sprite writes every pixel's depth */
    set(&c->prim, 6, 0x00);
    set(&c->rgbaq, 0, 0x01);
    set(&c->xyz2a, (uint64_t)((2048 - w / 2) << 4) | (uint64_t)((2048 - h / 2) << 4) << 16, 0x05);
    set(&c->xyz2b, (uint64_t)((2048 + w / 2) << 4) | (uint64_t)((2048 + h / 2) << 4) << 16, 0x05);
    set(&c->testb, test, 0x47);
}

int sceGsSetDefDBuff(void *p, int psm, int w, int h, int ztest, int zpsm, int clear) {
    DBuff *db = p;
    uint32_t bytes = (psm & 2) ? 2 : 4, pages = ((uint32_t)w * (uint32_t)h * bytes + 8191) / 8192;

    memset(db, 0, sizeof(*db));
    fill(db->giftag0, &db->draw0, &db->clear0, 0, pages * 2, psm, w, h, ztest, zpsm, clear);
    fill(db->giftag1, &db->draw1, &db->clear1, pages, pages * 2, psm, w, h, ztest, zpsm, clear);
    return 0;
}

int sceGsPutDrawEnv(uint64_t *giftag) {
    if (Gs_On() && !sWalk) {
        Port_GsGifChannel(Port_Addr(giftag), 1 + (uint32_t)(giftag[0] & 0x7FFF), 0);
    }
    return 0;
}

int sceGsSwapDBuff(void *p, int id) {
    DBuff *db = p;
    /* the buffer this frame is drawn into: what the renderer shows when the frame is finished */
    gGsMainFbp = (int)(((id & 1) ? db->draw1.frame.v : db->draw0.frame.v) & 0x1FF);
    return sceGsPutDrawEnv((id & 1) ? db->giftag1 : db->giftag0);
}

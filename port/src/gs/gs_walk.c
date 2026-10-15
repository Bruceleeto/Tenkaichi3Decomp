/*
 * The renderer's front end without the GS: what both builds need whether or not the PS2 reference (gs_core.c: GS
 * memory, the packets, the software rasteriser; gs_vu1.c) is linked. The PC build links the reference too
 * (PORT_GS_REF, from its Makefile) and hands it what only it can do; the Dreamcast build has this file alone.
 *   - which renderer draws, and starting it;
 *   - the count of pictures (gGsFrame);
 *   - the settings read from the environment (Port_GetEnv);
 *   - reading the shown picture back for the game (Gs_StoreImage).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "gs_internal.h"

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

/*
 * Graphics Synthesizer, PowerVR back end (BT3_GPU_API=pvr): the Dreamcast's graphics chip.
 *
 * gs_draw.c records the frame (every primitive as a GsDraw, the vertices, the decoded textures) and installs this
 * file as the sPvrBackend. It talks to the chip through the KOS PVR API (<dc/pvr.h>: pvr_scene_begin, the
 * polygon lists, pvr_mem_malloc), so the same code is meant to build for the Dreamcast against KOS itself. On PC
 * the API is port/third_party/gpu_pvr: KOS's PVR calls on libpvr, a software PowerVR, whose finished 640 x 480
 * picture is shown in the game's own window (pvr_host_attach).
 *
 * Built only with the Makefile's PVR=1 (the default there): -DBT3_PVR and gpu_pvr's include paths. Without it
 * this file is empty, so install.py's build is what it was. gpu_pvr's kos_pvr.cpp is GPL-2.0-only: a program
 * built with PVR=1 is not for release.
 *
 * State of it: the shell. Init, events, a scene per frame (background colour only), and the textures, targets
 * and pipelines the shared layer asks for, kept as records. Nothing of the recorded list is drawn yet.
 */
#ifdef BT3_PVR
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dc/pvr.h>
#include "pvr_host.h"
#include "gs_internal.h"
#include "gs_draw.h"
#include "ui.h"

static SDL_Window *sWindow;

/* ---- textures: a record per texture, its pixels in PVR memory once it is converted ------------------ */
typedef struct PvrTex {
    uint32_t w, h;
    int format;     /* GS_TEXFMT_* as the shared layer gave it */
    pvr_ptr_t mem;  /* NULL until converted (not yet: every texture draws white) */
} PvrTex;

static PvrTex sWhiteRec = {1, 1, GS_TEXFMT_RGBA8, NULL};

static GsTex tex_create(uint32_t w, uint32_t h, int format, int levels) {
    PvrTex *t = calloc(1, sizeof(PvrTex));
    (void)levels;
    if (t == NULL) { return 0; }
    t->w = w;
    t->h = h;
    t->format = format;
    return (GsTex)t;
}

static int tex_upload(GsTex tex, int level, uint32_t w, uint32_t h, int format, const void *px, uint32_t bytes) {
    (void)tex; (void)level; (void)w; (void)h; (void)format; (void)bytes;
    free((void *)px); /* (the pixels are ours: TODO convert them to a PVR format in PVR memory) */
    return 1;
}

static void tex_destroy(GsTex tex) {
    PvrTex *t = (PvrTex *)tex;
    if (t == NULL || t == &sWhiteRec) { return; }
    if (t->mem != NULL) { pvr_mem_free(t->mem); }
    free(t);
}

static int tex_slots_left(void) { return 1 << 20; }
static GsTex white_tex(void) { return (GsTex)&sWhiteRec; }

/* ---- render targets: only the shown frame buffer will exist on the chip; the rest are names ------------ */
static PvrTex sTgCol[MAX_TARGETS], sTgAux[MAX_TARGETS], sTgDep[MAX_TARGETS];

static void target_ensure(int i) { (void)i; }
static void target_drop(int i) { (void)i; }
static GsTex target_color(int i) { return (GsTex)&sTgCol[i]; }
static GsTex target_aux(int i) { return (GsTex)&sTgAux[i]; }
static GsTex target_depth(int i) { return (GsTex)&sTgDep[i]; }
static int target_read(int i, uint8_t *rgba) { (void)i; (void)rgba; return 0; }
static void target_copy(int src, int dst) { (void)src; (void)dst; }

/* ---- pipelines: the key of each (blend, depth, masks as gs_draw.c packs them), to become PVR headers ----- */
#define MAX_PIPES 4096
static uint32_t sPipeKey[MAX_PIPES];
static int sPipeCount;

static int pipe_get(uint32_t key) {
    int i;
    for (i = 0; i < sPipeCount; i++) {
        if (sPipeKey[i] == key) { return i; }
    }
    if (sPipeCount == MAX_PIPES) { return 0; }
    sPipeKey[sPipeCount] = key;
    return sPipeCount++;
}

static void scale_changed(void) {} /* the chip draws 640 x 480 whatever the scale */

/* ---- init and the frame ------------------------------------------------------------------------- */
static int pvr_be_init(void) {
    pvr_init_params_t params = {
        {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16}, /* opaque, translucent, punch-through */
        1024 * 1024, /* vertex buffer */
        0,           /* no vertex DMA */
        0,           /* no horizontal FSAA */
        1,           /* no autosort: translucent polygons in the order the GS drew them */
        3,           /* overflow bins */
        0,
    };

    sWindow = GsDraw_WindowCreate(0);
    if (sWindow == NULL) { return 0; }
    if (pvr_host_attach(sWindow) != 0) { return 0; }
    if (pvr_init(&params) != 0) {
        fprintf(stderr, "bt3: pvr: pvr_init failed\n");
        return 0;
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.25f); /* (dark blue: a frame went through, with nothing drawn yet) */
    fprintf(stderr, "bt3: PVR renderer: libpvr (software PowerVR), %u KB of PVR memory free\n", (unsigned)(pvr_mem_available() / 1024));
    return 1;
}

static void frame_end(void) {
    SDL_Event ev;
    int i;

    GsGpu_TitlePoll();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) { exit(0); }
        { extern void PortA11y_Event(const SDL_Event *ev); PortA11y_Event(&ev); } /* a11y.c: the repeat key and button */
        if (PortInput_Hotkey(&ev)) { continue; } /* (no settings window on this back end yet) */
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !ev.key.repeat) { exit(0); }
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F11) { GsDraw_FullscreenToggle(); }
    }
    if (getenv("BT3_GS_VERBOSE") != NULL && gGsFrame % 30 == 0) {
        fprintf(stderr, "pvr: frame %u: %u draws, %u vertices, %d targets, %d textures, %d pipelines\n",
                gGsFrame, gsDrawCount, gsVertCount + gsVuVertCount, gsTargetCount, gsTexCount, sPipeCount);
    }

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    /* TODO: the recorded list (gsDraws): 2D primitives first, then the vertex programs' draws */
    pvr_list_finish();
    pvr_scene_finish(); /* renders, and gpu_pvr shows the picture (kos_pvr_present) */

    for (i = 0; i < gsTargetCount; i++) {
        gsTargets[i].draws = 0;
    }
    gsVertCount = 0;
    gsVuVertCount = 0;
    gsVuIdxCount = 0;
    gsVuUniCount = 0;
    gsDrawCount = 0;
    gsAnchor = 0;
    gsSkipped = 0;
    gsNative = 0;
}

GsBackend sPvrBackend = {
    "pvr",
    pvr_be_init,
    frame_end,
    scale_changed,
    white_tex,
    tex_create,
    tex_upload,
    tex_destroy,
    tex_slots_left,
    target_ensure,
    target_drop,
    target_color,
    target_aux,
    target_depth,
    pipe_get,
    target_read,
    target_copy,
};
#endif

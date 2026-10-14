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
 * State of it: the 2D primitives (menus, HUD, text: the GS-pixel vertices of gsVerts) drawn into the shown frame
 * buffer, in the game's order (all in the translucent list, which keeps submission order with autosort off).
 * Textures arrive decoded to RGBA8 and are kept as ARGB4444 in PVR memory, resampled to a power of two from 8 to
 * 1024. Not drawn yet: the vertex programs' draws (3D), lines and points, draws into other frame buffers, draws
 * that sample a frame buffer, the native effects, the alpha and destination alpha tests, the scissor.
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

/* ---- textures: a record per texture, its pixels in PVR memory once converted -------------------------- */
typedef struct PvrTex {
    uint32_t w, h;  /* in PVR memory: powers of two, 8..1024 */
    int format;     /* GS_TEXFMT_* as the shared layer gave it */
    pvr_ptr_t mem;  /* NULL: draws white (not uploaded, a texture pack's compressed one, or PVR memory full) */
} PvrTex;

static PvrTex sWhiteRec = {8, 8, GS_TEXFMT_RGBA8, NULL};
static uint32_t sTexBytes, sTexPeak, sTexLive; /* in PVR memory now, the most so far; textures there */

static uint32_t pow2_in(uint32_t n) {
    uint32_t p = 8;
    while (p < n && p < 1024) { p <<= 1; }
    return p;
}

/* The PVR memory is taken here, so that a full memory is told to the shared cache (0), which then lets go of the
   textures no longer drawn and asks again. (A texture pack's compressed one gets none: it draws white.) */
static GsTex tex_create(uint32_t w, uint32_t h, int format, int levels) {
    PvrTex *t = calloc(1, sizeof(PvrTex));
    (void)levels;
    if (t == NULL) { return 0; }
    t->w = pow2_in(w);
    t->h = pow2_in(h);
    t->format = format;
    if (format == GS_TEXFMT_RGBA8) {
        t->mem = pvr_mem_malloc(t->w * t->h * 2);
        if (t->mem == NULL) {
            free(t);
            return 0;
        }
        sTexBytes += t->w * t->h * 2;
        sTexLive++;
        if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    }
    return (GsTex)t;
}

/* RGBA8 (alpha as the GS has it: 0x80 opaque) to ARGB4444, nearest-resampled to the record's size. The
   coordinates are 0..1 across the texture, so a resampled one is addressed as the original was. */
static int tex_upload(GsTex tex, int level, uint32_t w, uint32_t h, int format, const void *px, uint32_t bytes) {
    PvrTex *t = (PvrTex *)tex;
    const uint8_t *src = px;
    uint16_t *dst;
    uint32_t x, y;

    (void)bytes;
    if (t == NULL || t->mem == NULL || level != 0 || format != GS_TEXFMT_RGBA8 || w == 0 || h == 0) {
        free((void *)px);
        return 1;
    }
    dst = malloc(t->w * t->h * 2);
    if (dst != NULL) {
        for (y = 0; y < t->h; y++) {
            const uint8_t *row = src + (size_t)(y * h / t->h) * w * 4;
            for (x = 0; x < t->w; x++) {
                const uint8_t *c = row + (size_t)(x * w / t->w) * 4;
                uint32_t a = c[3] >= 0x80 ? 15 : c[3] >> 3; /* 0x80 = opaque */
                dst[y * t->w + x] = (uint16_t)(a << 12 | (c[0] >> 4) << 8 | (c[1] >> 4) << 4 | c[2] >> 4);
            }
        }
        pvr_txr_load(dst, t->mem, t->w * t->h * 2);
        free(dst);
    }
    free((void *)px);
    return 1;
}

static void tex_destroy(GsTex tex) {
    PvrTex *t = (PvrTex *)tex;
    if (t == NULL || t == &sWhiteRec) { return; }
    if (t->mem != NULL) { pvr_mem_free(t->mem); sTexBytes -= t->w * t->h * 2; sTexLive--; }
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
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    fprintf(stderr, "bt3: PVR renderer: libpvr (software PowerVR), %u KB of PVR memory free\n", (unsigned)(pvr_mem_available() / 1024));
    return 1;
}

/* The GS blend (A - B) * C + D as gs_draw.c keys it (bit 0 on, bits 1..3 the source factor, 4..6 the destination
   one: 0 zero, 1 / 5 one, 2 C, 4 1 - C, 3 subtracted; bits 7..8 C: source alpha, destination alpha, a constant).
   The PVR has no subtraction and no constant: a subtracted side is left out, a constant taken as 1. */
static int blend_factor(int k, int c, int dst) {
    if (k == 0 || k == 3) { return PVR_BLEND_ZERO; }
    if (k == 1 || k == 5) { return PVR_BLEND_ONE; }
    if (c == 2) { return k == 2 ? PVR_BLEND_ONE : PVR_BLEND_ZERO; }
    if (k == 2) { return c == 0 ? PVR_BLEND_SRCALPHA : PVR_BLEND_DESTALPHA; }
    (void)dst;
    return c == 0 ? PVR_BLEND_INVSRCALPHA : PVR_BLEND_INVDESTALPHA;
}

/* A GS colour channel (0x80 = 1.0, up to 0xFF) as the PVR's 0..255, clamped. */
static uint32_t gs_unit(uint32_t c) { return c >= 0x80 ? 255 : c * 2; }

/* A vertex being clipped: screen position, texture coordinate, colour (0..255 each). */
typedef struct ClipV {
    float x, y, u, v, c[4];
} ClipV;

/* Keeps the part of polygon `in` (n vertices) where side * (x or y) >= side * edge; returns the new count. */
static int clip_edge(const ClipV *in, int n, ClipV *out, int axis, float edge, float side) {
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const ClipV *a = &in[i], *b = &in[(i + 1) % n];
        float da = side * ((axis ? a->y : a->x) - edge), db = side * ((axis ? b->y : b->x) - edge);
        if (da >= 0.0f) { out[m++] = *a; }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float f = da / (da - db);
            int k;
            ClipV *o = &out[m++];
            o->x = a->x + (b->x - a->x) * f;
            o->y = a->y + (b->y - a->y) * f;
            o->u = a->u + (b->u - a->u) * f;
            o->v = a->v + (b->v - a->v) * f;
            for (k = 0; k < 4; k++) { o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * f; }
        }
    }
    return m;
}

static void put_vertex(const ClipV *c, int last) {
    pvr_vertex_t pv;
    pv.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    pv.x = c->x;
    pv.y = c->y;
    pv.z = 1.0f;
    pv.u = c->u;
    pv.v = c->v;
    pv.argb = (uint32_t)(c->c[3] + 0.5f) << 24 | (uint32_t)(c->c[0] + 0.5f) << 16 | (uint32_t)(c->c[1] + 0.5f) << 8 | (uint32_t)(c->c[2] + 0.5f);
    pv.oargb = 0;
    pvr_prim(&pv, sizeof(pv));
}

/* The 2D primitives drawn into the shown frame buffer, as triangles in the translucent list. Of the GS's tests:
   the alpha test as blending (a cut-out's transparent texels have alpha 0) or not drawn (never passes); the
   scissor by clipping the triangles here; the destination alpha test not yet (those draws are left out). */
static void draw_2d(int shown) {
    uint32_t n, k;
    const float sx = 640.0f / 512.0f, sy = 480.0f / 448.0f;

    for (n = 0; n < gsDrawCount; n++) {
        const GsDraw *d = &gsDraws[n];
        uint32_t key, bkey;
        pvr_poly_cxt_t cxt;
        pvr_poly_hdr_t hdr;
        const PvrTex *t;
        int tfx;

        if (getenv("BT3_PVR_LOG") != NULL && (int)gGsFrame == atoi(getenv("BT3_PVR_LOG"))) { /* every draw of one frame */
            const PvrTex *lt = d->mode[0] == 1 || d->mode[0] == 3 ? (const PvrTex *)d->tex : NULL;
            fprintf(stderr, "pvr: draw %u: native %d vu %d target %d%s first %u count %u key %08x mode %d %d %d %d tex %ux%u%s\n", n,
                    d->native, d->vu, d->target, d->target == shown ? " (shown)" : "", d->first, d->count,
                    d->pipeline >= 0 && d->pipeline < sPipeCount ? sPipeKey[d->pipeline] : 0, d->mode[0], d->mode[1], d->mode[2], d->mode[3],
                    lt ? lt->w : 0, lt ? lt->h : 0, lt && !lt->mem ? " (none)" : "");
        }
        if (d->native || d->vu || d->target != shown || d->pipeline < 0 || d->pipeline >= sPipeCount) { continue; }
        key = sPipeKey[d->pipeline];
        if (((key >> 13) & 3) != 0) { continue; }  /* lines, points */
        if ((key >> 16 & 7) == 0) { continue; }    /* writes no colour (alpha or depth only) */
        if (d->mode[0] == 2) { continue; }          /* samples a frame buffer */
        if (d->mode[3] == 1) { continue; }          /* alpha test NEVER (with KEEP: nothing is written) */
        if (d->misc[1] != 0.0f) { continue; }       /* destination alpha test: TODO */
        t = d->mode[0] != 0 ? (const PvrTex *)d->tex : NULL;
        if (t != NULL && t->mem != NULL) {
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED, (int)t->w, (int)t->h, t->mem,
                             (d->sampler & 1) ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
            cxt.txr.uv_clamp = (pvr_uv_clamp_t)(((d->sampler & 2) ? PVR_UVCLAMP_U : 0) | ((d->sampler & 4) ? PVR_UVCLAMP_V : 0));
            tfx = d->mode[1];
            cxt.txr.env = tfx == 1 ? (d->mode[2] ? PVR_TXRENV_REPLACE : PVR_TXRENV_DECAL) : PVR_TXRENV_MODULATEALPHA;
        } else {
            pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
            t = NULL;
        }
        bkey = key & 0x1FF;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = 0;
        if (bkey & 1) {
            int c = (int)((bkey >> 7) & 3);
            cxt.blend.src = (pvr_blend_mode_t)blend_factor((int)((bkey >> 1) & 7), c, 0);
            cxt.blend.dst = (pvr_blend_mode_t)blend_factor((int)((bkey >> 4) & 7), c, 1);
        } else if (d->mode[3] >= 6 && d->mode[3] <= 7) {
            /* alpha test GEQUAL / GREATER with blending off: a cut-out. Blending by the source alpha leaves its
               transparent texels (alpha 0) out, as the test does; partly transparent ones come out soft. */
            cxt.blend.src = PVR_BLEND_SRCALPHA;
            cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        } else {
            cxt.blend.src = PVR_BLEND_ONE;
            cxt.blend.dst = PVR_BLEND_ZERO;
        }
        pvr_poly_compile(&hdr, &cxt);
        pvr_prim(&hdr, sizeof(hdr));
        {
            /* the scissor (x, y, width, height at SCALE) in screen pixels */
            float x0 = (float)d->scissor[0] / (float)SCALE * sx, y0 = (float)d->scissor[1] / (float)SCALE * sy;
            float x1 = (float)(d->scissor[0] + d->scissor[2]) / (float)SCALE * sx, y1 = (float)(d->scissor[1] + d->scissor[3]) / (float)SCALE * sy;
            for (k = 0; k + 2 < d->count; k += 3) {
                ClipV a[8], b[8];
                int j, m = 3, inside = 1;
                for (j = 0; j < 3; j++) {
                    const Vtx *v = &gsVerts[d->first + k + j];
                    float q = v->q != 0.0f ? v->q : 1.0f;
                    a[j].x = v->x * sx;
                    a[j].y = v->y * sy;
                    a[j].u = t != NULL ? v->s / q : 0.0f;
                    a[j].v = t != NULL ? v->t / q : 0.0f;
                    a[j].c[0] = (float)gs_unit(v->r);
                    a[j].c[1] = (float)gs_unit(v->g);
                    a[j].c[2] = (float)gs_unit(v->b);
                    a[j].c[3] = (float)gs_unit(v->a);
                    if (a[j].x < x0 || a[j].x > x1 || a[j].y < y0 || a[j].y > y1) { inside = 0; }
                }
                if (!inside) {
                    m = clip_edge(a, m, b, 0, x0, 1.0f);
                    m = clip_edge(b, m, a, 0, x1, -1.0f);
                    m = clip_edge(a, m, b, 1, y0, 1.0f);
                    m = clip_edge(b, m, a, 1, y1, -1.0f);
                }
                /* a fan of triangles (at most 7 vertices after four edges) */
                for (j = 1; j + 1 < m; j++) {
                    put_vertex(&a[0], 0);
                    put_vertex(&a[j], 0);
                    put_vertex(&a[j + 1], 1);
                }
            }
        }
    }
}

static void frame_end(void) {
    SDL_Event ev;
    int i, shown;

    GsGpu_TitlePoll();
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) { exit(0); }
        { extern void PortA11y_Event(const SDL_Event *ev); PortA11y_Event(&ev); } /* a11y.c: the repeat key and button */
        if (PortInput_Hotkey(&ev)) { continue; } /* (no settings window on this back end yet) */
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !ev.key.repeat) { exit(0); }
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F11) { GsDraw_FullscreenToggle(); }
    }
    if (getenv("BT3_GS_VERBOSE") != NULL && gGsFrame % 30 == 0) {
        fprintf(stderr, "pvr: frame %u: %u draws, %u vertices, %d targets, %d textures (%u KB in PVR memory, at most %u KB), %d pipelines\n",
                gGsFrame, gsDrawCount, gsVertCount + gsVuVertCount, gsTargetCount, gsTexCount, sTexBytes >> 10, sTexPeak >> 10, sPipeCount);
    }

    /* the shown buffer: the one sceGsSwapDBuff set up for this frame, else the first target (as gs_gl.c) */
    shown = gGsMainFbp >= 0 ? GsDraw_TargetGet((uint32_t)gGsMainFbp, 0) : -1;
    if (shown < 0 && gsTargetCount > 0) { shown = 0; }

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    if (shown >= 0) { draw_2d(shown); }
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

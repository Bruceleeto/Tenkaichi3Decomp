/*
 * Graphics Synthesizer, PowerVR back end (BT3_GPU_API=pvr): the Dreamcast's graphics chip.
 *
 * gs_draw.c records the frame (every primitive as a GsDraw, the vertices, the decoded textures) and installs this
 * file as the sPvrBackend. It talks to the chip through the KOS PVR API (<dc/pvr.h>: pvr_scene_begin, the
 * polygon lists, pvr_mem_malloc), so the same code is meant to build for the Dreamcast against KOS itself. On PC
 * the API is port/third_party/gpu_pvr: KOS's PVR calls on libpvr, a software PowerVR, whose finished 640 x 480
 * picture is shown in the game's own window (pvr_host_attach). On the Dreamcast (__DREAMCAST__) it is KOS.
 *
 * Built only with the Makefile's PVR=1 (the default there): -DBT3_PVR and gpu_pvr's include paths. Without it
 * this file is empty, so install.py's build is what it was. gpu_pvr's kos_pvr.cpp is GPL-2.0-only: a program
 * built with PVR=1 is not for release.
 *
 * State of it: what is drawn into the shown frame buffer, in the game's order (all in the translucent list,
 * which keeps submission order with autosort off): the 2D primitives (menus, HUD, text: the GS-pixel vertices of
 * gsVerts) and the vertex programs' draws (3D), whose arithmetic (shaders/vu0, vu4, vu6.vert) is done here on the
 * CPU, clipped against the eye and the depth range, depth as 1 / w. Textures arrive decoded to RGBA8 and are kept
 * as ARGB4444 in PVR memory, resampled to a power of two from 8 to 1024. The other frame buffers are textures
 * rendered into (render to texture, ARGB4444), so the effects built from them draw (the fighters' shadow). Not
 * drawn yet: lines and points, the native effects, the table passes, the 2D destination alpha test.
 * TODO: the glare and the blur of distant things are left out: they are made from an alpha a table pass puts
 * there from the depth, which the PVR cannot read (frame_render). It could be made per vertex from the depth.
 */
#ifdef BT3_PVR
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dc/pvr.h>
#include "gs_internal.h"
#include "texpak.h"
#include "../port_addr.h"
#include "gs_draw.h"
#include "ui.h"
#include "../../../include/port/rnd.h" /* Rnd_FxOn, RndDirect */
#ifndef __DREAMCAST__
#include "pvr_host.h" /* gpu_pvr: the window the picture is shown in */
#else
#include <dc/sq.h> /* the movie's macroblocks to the YUV converter */
static uint32_t sFbCfg2; /* the pixel format the PVR writes, as vid_set_mode left it (the screen's RGB565) */
#endif

/* The next render (pvr_scene_begin_rtt) writes ARGB4444 instead of RGB565. KOS has no call for it: the PVR's
   register, which a render to the screen puts back. */
static void rtt_argb4444(void) {
#ifdef __DREAMCAST__
    PVR_SET(PVR_FB_CFG_2, (sFbCfg2 & ~7u) | 2u);
#else
    kos_pvr_rtt_argb4444();
#endif
}

/* What a scene may send: the vertex buffer pvr_init was given, less a margin (the background polygon, and the
   TA's own layout of a polygon being a little larger than what is sent). kos_pvr does not stop at its end: what
   goes past it lands on the frame buffers and the textures. So a frame with more than this loses triangles. */
#define VERTEX_BUF_BYTES (1536 * 1024)
#define SCENE_BUDGET (VERTEX_BUF_BYTES - 128 * 1024)
static uint32_t sSent, sTris, sDropped; /* this scene: bytes sent, triangles sent, triangles left out */

/* What one render draws in: GS pixels to its pixels (the screen: 640 x 480 for the GS's 512 x 448; a frame buffer
   rendered to a texture: 1:1), its size, and the texture coordinate's scale for the draw being sent (a frame
   buffer as a texture: the shared layer's coordinates are in GS_W x GS_H texels of the buffer, the PVR's in its
   texture's own size). */
static float sSx = 640.0f / 512.0f, sSy = 480.0f / 448.0f, sRw = 640.0f, sRh = 480.0f, sUs = 1.0f, sVs = 1.0f;

#define GUARD 1024.0f
/* The PVR handles screen edges, but extreme coordinates still need a guard band. */
static inline int screen_scissor(float x0, float y0, float x1, float y1) {
#if defined(__DREAMCAST__) || defined(BT3_DCSIM)
    return x0 <= 0.0f && y0 <= 0.0f && x1 >= sRw && y1 >= sRh;
#else
    return 0;
#endif
}

static inline int scissor_inside(float x, float y, float x0, float y0, float x1, float y1) {
    if (screen_scissor(x0, y0, x1, y1)) {
        return x >= -GUARD && x <= sRw + GUARD && y >= -GUARD && y <= sRh + GUARD;
    }
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}
/* The GS's constant blend factor (ALPHA.FIX) of the draw being sent, as its vertices' alpha (below); < 0: none. */
static float sFixA = -1.0f;


/* ---- textures: a record per texture, its pixels in PVR memory once converted -------------------------- */
typedef struct PvrTex {
    uint32_t w, h;  /* the image: powers of two, 8..1024 */
    int format;     /* GS_TEXFMT_* as the shared layer gave it */
    pvr_ptr_t mem;  /* NULL: draws white (not uploaded, a texture pack's compressed one, or PVR memory full) */
    uint32_t bytes; /* of PVR memory at mem */
    /* As the PVR is given it: plain ARGB4444 (w x h, not twiddled), or VQ (2w x 2h, twiddled, `base` before `mem`
       by the codebook entries it does not use; see tex_vq) */
    uint32_t pw, ph, fmt;
    pvr_ptr_t base;
} PvrTex;

static PvrTex sWhiteRec = {8, 8, GS_TEXFMT_RGBA8, NULL, 0, 8, 8, 0, NULL};
static uint32_t sTexBytes, sTexPeak, sTexLive; /* in PVR memory now, the most so far; textures there */

/* PVR memory of a texture the game is done with, given back two frames later (late_free): draws already sent may
   still read it. A frame's draws are sent when the lists are full (spill) and the render runs while the next frame
   is made; a texture made in the freed memory the same frame showed in the earlier draws. KOS's pvr_wait_ready
   returns once the scene before has begun rendering, so at the end of frame N the render of N - 2 has finished. */
#define LATE_MAX 1024
static pvr_ptr_t sLate[3][LATE_MAX];
static unsigned sLateCount[3], sLateCur;

static void late_free(pvr_ptr_t p) {
    if (sLateCount[sLateCur] == LATE_MAX) { /* (more than a scene's textures at once) */
        static int told;
        if (!told++) { fprintf(stderr, "bt3: pvr: more than %d textures freed in a frame: the rest at once\n", LATE_MAX); }
        pvr_mem_free(p);
        return;
    }
    sLate[sLateCur][sLateCount[sLateCur]++] = p;
}

/* At a frame's end: what frame N - 2 freed. */
static void late_release(void) {
    unsigned i, old = (sLateCur + 1) % 3;
    for (i = 0; i < sLateCount[old]; i++) { pvr_mem_free(sLate[old][i]); }
    sLateCount[old] = 0;
    sLateCur = old;
}
static int sNatOn; /* the game's texture entries are PVR textures of their own (below: NatTex); BT3_TEXPAK= none */
/* (and only where the list is not read for its packets: there every upload is sent as on the PS2, and drawn from
   GS memory) */
extern int Port_ListRead(void); /* gs_walk.c */
static int nat_on(void) { return sNatOn && !Port_ListRead(); }
void Port_Bind(int kind, uint32_t block, uint32_t blocks, uint32_t h); /* port/src/gs_marker.c */
static unsigned sNatWhy[5]; /* records not native: not 4/8-bit, packet not read, not in the pack, another size; no record free */
static unsigned sNatEvicted; /* native textures that let go of their PVR memory (nat_evict) */
static unsigned sTintDraws; /* per 30 frames: draws with a tinted palette (Port_ClutTint) */
static unsigned sPalMade[2]; /* per 30 frames: palettes made for a native one from tex.pak, out of GS memory */
static unsigned sNatLive, sNatSteal, sNatMiss[4]; /* per frame, draws of a 4/8-bit texture not native: no record at
                                                     its block, record not native, another size or format; uploads not native */
extern unsigned gRndBindLive, gRndBindPeak, gRndRebound; /* port/src/gs_marker.c */
static unsigned sPalMiss; /* per 30 frames: palette uploads not native (not a palette of the pack, or no record free): sent
                             to GS memory as before */
static int sPalAsk;       /* Port_TexNative asking whether a direct entry's pixels are also a palette (not counted) */

/* pvr_mem_malloc, telling what did not fit and what PVR memory holds then (game textures as converted, as the
   old path makes them, render targets). */
static int nat_evict(void);
static void *pvr_alloc(uint32_t bytes, const char *what, uint32_t addr) {
    void *p = pvr_mem_malloc(bytes);
    while (p == NULL && nat_evict()) { /* (the native textures drawn longest ago let go, until it fits) */
        p = pvr_mem_malloc(bytes);
    }
    if (p == NULL) {
        fprintf(stderr, "pvr: frame %u: no PVR memory for %u bytes (%s %08X); there: %u KB of textures (%u native, %u the old way), %u KB free\n",
                (unsigned)gGsFrame, (unsigned)bytes, what, (unsigned)addr, (unsigned)(sTexBytes / 1024), sNatLive, sTexLive,
                (unsigned)(pvr_mem_available() / 1024));
    }
    return p;
}

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
        t->bytes = t->w * t->h * 2;
        t->mem = pvr_alloc(t->bytes, "a texture the old way", 0);
        if (t->mem == NULL) {
            free(t);
            return 0;
        }
        t->pw = t->w;
        t->ph = t->h;
        t->fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
        t->base = t->mem;
        sTexBytes += t->bytes;
        sTexLive++;
        if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    }
    return (GsTex)t;
}

/* The PVR's twiddled order (libpvr's TextureSampler::twiddle_address): u and v interleaved over the smaller side,
   the rest of the larger one above. */
static uint32_t spread_bits(uint32_t x) {
    x &= 0x3FF;
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

static uint32_t twiddle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, bits = 0, s, idx;
    for (s = m; s > 1; s >>= 1) { bits++; }
    idx = spread_bits(v & (m - 1)) | (spread_bits(u & (m - 1)) << 1);
    if (w != h) { idx |= ((w > h ? u : v) >> bits) << (2 * bits); }
    return idx;
}

/* A texture of at most 256 colours as VQ, without loss: the PVR's VQ texture of twice the size, one index per texel
   of the image, each codebook entry four texels of one colour (the 2 x 2 block an index covers). Half the memory of
   ARGB4444 and a codebook of 8 bytes per colour; no palette RAM (its 64 banks are shared by the whole screen). A
   codebook of n colours is the end of a full one of 256 (as iortcw's pvrtex files): the indices start at 256 - n
   and the PVR is pointed that much before it. The coordinates stay 0..1 across the texture. (Bilinear filtering
   blends between texels over half the distance it does on the plain one.) Returns 0 when the texture has more
   colours, or no memory came free for it (it stays ARGB4444). */
static int tex_vq(PvrTex *t, const uint16_t *px) {
    enum { SLOTS = 1024 };
    uint16_t key[SLOTS], color[256];
    uint8_t *out;
    int16_t slot[SLOTS];
    uint32_t n = 0, i, bytes, first;
    pvr_ptr_t mem;

    if (t->w > 512 || t->h > 512 || getenv("BT3_PVR_TEX") != NULL) { return 0; } /* (BT3_PVR_TEX=4444: none, for comparing) */
    memset(slot, -1, sizeof(slot));
    out = malloc(t->w * t->h);
    if (out == NULL) { return 0; }
    for (i = 0; i < t->w * t->h; i++) { /* the colours, by an open hash of the 16-bit values */
        uint32_t h = ((uint32_t)px[i] * 40503u >> 6) & (SLOTS - 1);
        while (slot[h] >= 0 && key[h] != px[i]) { h = (h + 1) & (SLOTS - 1); }
        if (slot[h] < 0) {
            if (n == 256) { free(out); return 0; }
            key[h] = px[i];
            slot[h] = (int16_t)n;
            color[n++] = px[i];
        }
        out[i] = (uint8_t)slot[h];
    }
    bytes = n * 8 + t->w * t->h;
    if (bytes >= t->bytes) { free(out); return 0; }
    first = 256 - n;
    {   /* the indices in twiddled order, after the codebook */
        uint8_t *buf = malloc(bytes);
        uint32_t x, y;
        if (buf == NULL) { free(out); return 0; }
        for (i = 0; i < n; i++) {
            uint16_t *e = (uint16_t *)(buf + i * 8);
            e[0] = e[1] = e[2] = e[3] = color[i];
        }
        for (y = 0; y < t->h; y++) {
            for (x = 0; x < t->w; x++) {
                buf[n * 8 + twiddle_index(x, y, t->w, t->h)] = (uint8_t)(first + out[y * t->w + x]);
            }
        }
        free(out);
        pvr_mem_free(t->mem); /* the smaller block instead (freed first: it fits where the larger one was) */
        sTexBytes -= t->bytes;
        mem = pvr_alloc(bytes, "an old-way texture as VQ", 0);
        if (mem == NULL) { /* (cannot happen: the larger block was just freed) */
            t->mem = NULL;
            free(buf);
            return 1;
        }
        pvr_txr_load(buf, mem, bytes);
        free(buf);
    }
    t->mem = mem;
    t->bytes = bytes;
    sTexBytes += bytes;
    t->pw = t->w * 2;
    t->ph = t->h * 2;
    t->fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
    t->base = (pvr_ptr_t)((uint8_t *)mem - first * 8);
    return 1;
}

/* RGBA8 (alpha as the GS has it: 0x80 opaque) to ARGB4444, nearest-resampled to the record's size, then VQ when it
   has few enough colours (tex_vq). The coordinates are 0..1 across the texture, so a resampled one is addressed as
   the original was. */
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
    if (t->bytes != t->w * t->h * 2) { /* uploaded again after it was made VQ: the full block back */
        pvr_mem_free(t->mem);
        sTexBytes -= t->bytes;
        t->bytes = t->w * t->h * 2;
        t->mem = pvr_alloc(t->bytes, "an old-way texture again", 0);
        t->pw = t->w;
        t->ph = t->h;
        t->fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
        t->base = t->mem;
        if (t->mem == NULL) { free((void *)px); return 1; }
        sTexBytes += t->bytes;
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
        if (!tex_vq(t, dst)) {
            pvr_txr_load(dst, t->mem, t->w * t->h * 2);
        }
        free(dst);
    }
    free((void *)px);
    return 1;
}

static void tex_destroy(GsTex tex) {
    PvrTex *t = (PvrTex *)tex;
    if (t == NULL || t == &sWhiteRec) { return; }
    if (t->mem != NULL) { late_free(t->mem); sTexBytes -= t->bytes; sTexLive--; }
    free(t);
}

static int tex_slots_left(void) { return 1 << 20; }
static GsTex white_tex(void) { return (GsTex)&sWhiteRec; }

/* ---- render targets: only the shown frame buffer will exist on the chip; the rest are names ------------ */
static PvrTex sTgCol[MAX_TARGETS], sTgAux[MAX_TARGETS], sTgDep[MAX_TARGETS];

static void target_ensure(int i) { (void)i; }
static void target_drop(int i);
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
        VERTEX_BUF_BYTES, /* vertex buffer: a demo fight frame sends at most about 1.1 MB (as strips); each byte here costs two of the texture heap */
        0,           /* no vertex DMA */
        0,           /* no horizontal FSAA */
        1,           /* no autosort: translucent polygons in the order the GS drew them */
        3,           /* overflow bins */
        0,
    };

#ifndef __DREAMCAST__
    {
        struct SDL_Window *window = GsDraw_WindowCreate(0);
        if (window == NULL || pvr_host_attach(window) != 0) { return 0; }
    }
#endif
    if (pvr_init(&params) != 0) {
        fprintf(stderr, "bt3: pvr: pvr_init failed\n");
        return 0;
    }
#ifdef __DREAMCAST__
    sFbCfg2 = PVR_GET(PVR_FB_CFG_2);
#endif
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_pal_format(PVR_PAL_ARGB4444);
    sNatOn = getenv("BT3_TEXPAK") == NULL || getenv("BT3_TEXPAK")[0] != '\0';
    fprintf(stderr, "bt3: PVR renderer: libpvr (hardware PowerVR, on the GPU), %u KB of PVR memory free\n", (unsigned)(pvr_mem_available() / 1024));
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

/* The PVR's depth for a GS depth (0..1 of the depth buffer's maximum, bigger is nearer, as the PVR's): kept above
   the background plane (KOS puts it at 0.0001). 2D and 3D share it, so the GS's depth tests between them hold.
   (The PVR also takes it as 1 / w for perspective-correct texturing; the GS's depth is a + b / w, close to that
   except near the far plane.) */
#define PVR_Z(gsz) (0.001f + (gsz))

/* A vertex being clipped: screen position, the PVR's depth (PVR_Z), texture coordinate, colour (0..255 each).
   While clipped against the scissor, u, v and the colour are held multiplied by z (perspective-correct). */
typedef struct ClipV {
    float x, y, z, u, v, c[4];
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
            o->z = a->z + (b->z - a->z) * f;
            o->u = a->u + (b->u - a->u) * f;
            o->v = a->v + (b->v - a->v) * f;
            for (k = 0; k < 4; k++) { o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * f; }
        }
    }
    return m;
}


/* The game's tints of a fighter's palettes (src/sys/gfxm_d_c.c ObjDraw_AddClutPass: on the PS2, sprites blended over
   them in GS memory), made the PVR's way: every colour of the palette becomes colour * k + o, which the texture's
   draws get as their vertex colour times k and the offset (specular) colour o times the vertex colour; `alpha` >= 0:
   the palette's alpha replaced (the texture's alpha off, the vertices' times it). Per frame. As the GS layer has
   it (gGsTint): k (0..255 = 0..1) in bits 0..23, o in 24..47, the alpha in 48..55 with bit 56, bit 63 set. */
typedef struct NatTint { float k[3], o[3]; int alpha; } NatTint;
static NatTint sTintNow;
static const NatTint *sTintCur; /* the draw being sent: &sTintNow, or NULL */
static int sTintFlat;           /* and it is a decal (the vertex colour not taken): its vertices as white */

/* (written where the TA takes it, KOS's direct rendering: the store queue, no copy; the list's pvr_list_begin has
   them pointed at the TA) */
static void put_vertex(const ClipV *c, int last) {
    pvr_vertex_t *pv = (pvr_vertex_t *)pvr_dr_target();
    uint32_t argb = (uint32_t)(c->c[3] + 0.5f) << 24 | (uint32_t)(c->c[0] + 0.5f) << 16 | (uint32_t)(c->c[1] + 0.5f) << 8 | (uint32_t)(c->c[2] + 0.5f);
    uint32_t oargb = 0;
    if (sTintCur != NULL) {
        const NatTint *t = sTintCur;
        float r = sTintFlat ? 255.0f : c->c[0], g = sTintFlat ? 255.0f : c->c[1], b = sTintFlat ? 255.0f : c->c[2];
        float a = sTintFlat ? 255.0f : c->c[3];
        if (t->alpha >= 0) { a = a * (float)t->alpha / 255.0f; }
        argb = (uint32_t)(a + 0.5f) << 24 | (uint32_t)(r * t->k[0] + 0.5f) << 16 | (uint32_t)(g * t->k[1] + 0.5f) << 8 | (uint32_t)(b * t->k[2] + 0.5f);
        oargb = (uint32_t)(t->o[0] * r / 255.0f + 0.5f) << 16 | (uint32_t)(t->o[1] * g / 255.0f + 0.5f) << 8 | (uint32_t)(t->o[2] * b / 255.0f + 0.5f);
    }
    pv->flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    pv->x = c->x;
    pv->y = c->y;
    pv->z = c->z;
    pv->u = c->u;
    pv->v = c->v;
    pv->argb = argb;
    pv->oargb = oargb;
    pvr_dr_commit(pv);
}

/* A triangle (or what is left of it) in screen pixels, clipped against the scissor rectangle and sent as a fan. */
static void emit_poly(ClipV *a, int m, float x0, float y0, float x1, float y1) {
    ClipV b[16];
    int j, k, inside = 1;

    if (screen_scissor(x0, y0, x1, y1)) {
        x0 = y0 = -GUARD;
        x1 = sRw + GUARD;
        y1 = sRh + GUARD;
    }
    for (j = 0; j < m; j++) {
        if (a[j].x < x0 || a[j].x > x1 || a[j].y < y0 || a[j].y > y1) { inside = 0; }
    }
    if (!inside) {
        for (j = 0; j < m; j++) { /* perspective-correct while clipped: the attributes times 1 / w */
            a[j].u *= a[j].z; a[j].v *= a[j].z;
            for (k = 0; k < 4; k++) { a[j].c[k] *= a[j].z; }
        }
        m = clip_edge(a, m, b, 0, x0, 1.0f);
        m = clip_edge(b, m, a, 0, x1, -1.0f);
        m = clip_edge(a, m, b, 1, y0, 1.0f);
        m = clip_edge(b, m, a, 1, y1, -1.0f);
        for (j = 0; j < m; j++) {
            float r = a[j].z > 0.0f ? 1.0f / a[j].z : 0.0f;
            a[j].u *= r; a[j].v *= r;
            for (k = 0; k < 4; k++) { a[j].c[k] *= r; }
        }
    }
    for (j = 1; j + 1 < m; j++) {
        if (sSent + 3 * sizeof(pvr_vertex_t) > SCENE_BUDGET) {
            sDropped++;
            continue;
        }
        sSent += 3 * sizeof(pvr_vertex_t);
        sTris++;
        put_vertex(&a[0], 0);
        put_vertex(&a[j], 0);
        put_vertex(&a[j + 1], 1);
    }
}

/* ---- the vertex programs on the CPU (shaders/vu0.vert, vu4.vert, vu6.vert: the same arithmetic) ---------- */
/* A vertex in clip space (the screen matrix's result s) with its attributes. */
typedef struct ClipH {
    float s[4], u, v, c[4];
} ClipH;

/* m * (x, y, z, w), m a std140 mat4 (column-major) */
static void mat_mul(const float *m, const float *v, float *out) {
    int r;
    for (r = 0; r < 4; r++) {
        out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    }
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* One vertex of a vertex-program draw: vu 1 = program 0 (fighters, its layers in misc.w), 2 = program 4
   (stage), 3 = program 6 (ground shadow). Colours stay in GS units (0x80 = 1.0) until they are sent. */
static void vu_vertex(int vu, const Vu0Uniform *U, const float *in, ClipH *o) {
    const float *pos = in, *at = in + 4, *st = in + 8;
    int k;

    if (vu == 1) {
        float layer = U->misc[3], w = layer == 4.0f ? 1.0f : pos[3], pa[4], pb[4], qa[4], qb[4], p[4];
        qa[0] = pos[0] - U->pivotA[0]; qa[1] = pos[1] - U->pivotA[1]; qa[2] = pos[2] - U->pivotA[2]; qa[3] = 1.0f;
        qb[0] = pos[0] - U->pivotB[0]; qb[1] = pos[1] - U->pivotB[1]; qb[2] = pos[2] - U->pivotB[2]; qb[3] = 1.0f;
        mat_mul(U->boneA, qa, pa);
        mat_mul(U->boneB, qb, pb);
        for (k = 0; k < 3; k++) { p[k] = pb[k] + (pa[k] - pb[k]) * w; }
        p[3] = 1.0f;
        mat_mul(U->screen, p, o->s);
        if (layer == 2.0f && o->s[3] < 0.0f) { for (k = 0; k < 4; k++) { o->s[k] = -o->s[k]; } } /* the shadow camera's negative w */
        if (layer == 5.0f) {
            for (k = 0; k < 4; k++) { o->c[k] = U->color1[k]; }
            o->u = (U->light[0] * at[0] + U->light[1] * at[1] + U->light[2] * at[2] + U->light[3]) * 0.5f + 0.5f;
            o->v = (U->light2[0] * at[0] + U->light2[1] * at[1] + U->light2[2] * at[2] + U->light2[3]) * 0.5f + 0.5f;
        } else if (layer >= 3.0f) {
            for (k = 0; k < 3; k++) { o->c[k] = clampf(floorf(at[k]), 0.0f, 255.0f); }
            o->c[3] = layer == 4.0f ? U->color0[3] : clampf(floorf(at[3]), 0.0f, 255.0f);
            o->u = st[0]; o->v = st[1];
        } else if (layer != 1.0f) {
            for (k = 0; k < 4; k++) { o->c[k] = U->color0[k]; }
            o->u = st[0]; o->v = st[1];
        } else { /* the toon layer: the normal turned by each bone, against the light */
            float na = 0.0f, nb = 0.0f;
            int r;
            for (r = 0; r < 3; r++) {
                float ra = U->boneA[r] * at[0] + U->boneA[4 + r] * at[1] + U->boneA[8 + r] * at[2];
                float rb = U->boneB[r] * at[0] + U->boneB[4 + r] * at[1] + U->boneB[8 + r] * at[2];
                na += U->light[r] * ra;
                nb += U->light[r] * rb;
            }
            for (k = 0; k < 4; k++) { o->c[k] = U->color1[k]; }
            o->u = (nb + (na - nb) * w + U->light[3]) * 0.5f + 0.5f;
            o->v = 0.0f;
        }
    } else if (vu == 2) {
        mat_mul(U->screen, pos, o->s);
        for (k = 0; k < 4; k++) { o->c[k] = clampf(floorf(at[k]), 0.0f, 255.0f); }
        o->u = st[0]; o->v = st[1];
    } else {
        float p[4] = {pos[0], pos[1], pos[2], 1.0f}, c[4];
        mat_mul(U->screen, p, o->s);
        mat_mul(U->boneA, p, c); /* the shadow camera; pivotA.x its scale, pivotB.xy the texture's share of its buffer */
        o->u = (c[0] * U->pivotA[0] + 1.0f) * 0.5f * U->pivotB[0];
        o->v = (c[1] * U->pivotA[0] + 1.0f) * 0.5f * U->pivotB[1];
        for (k = 0; k < 4; k++) { o->c[k] = clampf(floorf(at[k]), 0.0f, 255.0f); }
    }
}

/* Keeps the part of a clip-space polygon where dist >= 0 (dist = s . plane); returns the new count. */
static int clip_plane(const ClipH *in, int n, ClipH *out, const float *plane) {
    int i, m = 0, k;
    for (i = 0; i < n; i++) {
        const ClipH *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->s[0] * plane[0] + a->s[1] * plane[1] + a->s[2] * plane[2] + a->s[3] * plane[3] + plane[4];
        float db = b->s[0] * plane[0] + b->s[1] * plane[1] + b->s[2] * plane[2] + b->s[3] * plane[3] + plane[4];
        if (da >= 0.0f) { out[m++] = *a; }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float f = da / (da - db);
            ClipH *o = &out[m++];
            for (k = 0; k < 4; k++) { o->s[k] = a->s[k] + (b->s[k] - a->s[k]) * f; }
            o->u = a->u + (b->u - a->u) * f;
            o->v = a->v + (b->v - a->v) * f;
            for (k = 0; k < 4; k++) { o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * f; }
        }
    }
    return m;
}

/* A vertex-program triangle: clipped in clip space against the eye (w > 0) and a guard band around
   the screen (as a GPU does: a vertex just in front of the eye would otherwise land millions of pixels away, past
   what the PVR's edge arithmetic holds), then projected to screen pixels, then the scissor. Depth is clamped, not
   clipped: the PC back ends turn the GPU's depth clipping off for the same reason (the sky and far scenery lie
   beyond the GS's depth range and are drawn all the same). */
/* A clip-space vertex in screen pixels, the PVR's depth and colour. */
static void vu_project(const ClipH *a, const Vu0Uniform *U, ClipV *o) {
    const float sx = sSx, sy = sSy;
    float zmax = U->misc[2] > 0.0f ? U->misc[2] : -U->misc[2], rw = 1.0f / a->s[3];
    int k;
    o->x = (a->s[0] * rw - U->misc[0]) * sx;
    o->y = (a->s[1] * rw - U->misc[1]) * sy;
    o->z = PVR_Z(zmax > 0.0f ? clampf(a->s[2] * rw / zmax, 0.0f, 1.0f) : rw);
    o->u = a->u * sUs;
    o->v = a->v * sVs;
    for (k = 0; k < 4; k++) { o->c[k] = (float)gs_unit((uint32_t)clampf(a->c[k], 0.0f, 255.0f)); }
    if (sFixA >= 0.0f) { o->c[3] = sFixA * 255.0f; }
}

static void vu_triangle(int vu, const Vu0Uniform *U, const uint32_t *idx, float x0, float y0, float x1, float y1) {
    ClipH a[16], b[16];
    ClipV out[16];
    int j, m = 3;
    const float eye[5] = {0.0f, 0.0f, 0.0f, 1.0f, -1e-3f};
    const float sx = sSx, sy = sSy;
    /* screen x = (s.x / w - misc.x) * sx within [-GUARD, 640 + GUARD], y alike */
    const float left[5] = {1.0f, 0.0f, 0.0f, -(U->misc[0] - GUARD / sx), 0.0f};
    const float right[5] = {-1.0f, 0.0f, 0.0f, U->misc[0] + (sRw + GUARD) / sx, 0.0f};
    const float top[5] = {0.0f, 1.0f, 0.0f, -(U->misc[1] - GUARD / sy), 0.0f};
    const float bottom[5] = {0.0f, -1.0f, 0.0f, U->misc[1] + (sRh + GUARD) / sy, 0.0f};

    for (j = 0; j < 3; j++) { vu_vertex(vu, U, &gsVuVerts[(size_t)idx[j] * 12], &a[j]); }
    m = clip_plane(a, m, b, eye);
    m = clip_plane(b, m, a, left);
    m = clip_plane(a, m, b, right);
    m = clip_plane(b, m, a, top);
    m = clip_plane(a, m, b, bottom);
    if (m < 3) { return; }
    for (j = 0; j < m; j++) { vu_project(&b[j], U, &out[j]); }
    emit_poly(out, m, x0, y0, x1, y1);
}

/* The vertex programs' strips, sent as the PVR's strips: one vertex per triangle instead of three. The shared
   front end hands them on as triangles (i, i+1, i+2) of the strip's vertices, leaving out the ones the game
   flags (gs_draw.c, GsGpu_DrawVu0/4/6), so a triangle whose first index follows the last one's continues the
   strip. One that needs clipping (behind the eye, outside the scissor) ends it and goes through vu_triangle. */
static struct { ClipV held; int n; } sStrip; /* the strip being sent: its last vertex is held until the next */

static void strip_end(void) {
    if (sStrip.n > 0) { put_vertex(&sStrip.held, 1); }
    sStrip.n = 0;
}

static void strip_add(const ClipV *v) {
    if (sStrip.n > 0) { put_vertex(&sStrip.held, 0); }
    sStrip.held = *v;
    sStrip.n++;
}

/* A vertex that needs no clipping: in front of the eye, inside the scissor. */
static int vu_plain(int vu, const Vu0Uniform *U, uint32_t i, ClipV *o, float x0, float y0, float x1, float y1) {
    ClipH h;
    vu_vertex(vu, U, &gsVuVerts[(size_t)i * 12], &h);
    if (h.s[3] < 1e-3f) { return 0; }
    vu_project(&h, U, o);
    return scissor_inside(o->x, o->y, x0, y0, x1, y1);
}

static void vu_draw(int vu, const Vu0Uniform *U, const uint32_t *idx, uint32_t count, float x0, float y0, float x1, float y1) {
    uint32_t k, prev = 0;
    ClipV v[3];

    for (k = 0; k + 2 < count; k += 3) {
        const uint32_t *ix = &idx[k];
        if (sStrip.n > 0 && ix[0] == prev + 1) { /* the next triangle of the strip: one more vertex */
            if (sSent + sizeof(pvr_vertex_t) <= SCENE_BUDGET && vu_plain(vu, U, ix[2], &v[2], x0, y0, x1, y1)) {
                strip_add(&v[2]);
                sSent += sizeof(pvr_vertex_t);
                sTris++;
                prev = ix[0];
                continue;
            }
        } else if (sSent + 3 * sizeof(pvr_vertex_t) <= SCENE_BUDGET && vu_plain(vu, U, ix[0], &v[0], x0, y0, x1, y1) &&
                   vu_plain(vu, U, ix[1], &v[1], x0, y0, x1, y1) && vu_plain(vu, U, ix[2], &v[2], x0, y0, x1, y1)) {
            strip_end(); /* a new strip */
            strip_add(&v[0]);
            strip_add(&v[1]);
            strip_add(&v[2]);
            sSent += 3 * sizeof(pvr_vertex_t);
            sTris++;
            prev = ix[0];
            continue;
        }
        strip_end();
        vu_triangle(vu, U, ix, x0, y0, x1, y1);
    }
    strip_end();
}

/* The 2D primitives drawn into the shown frame buffer, as triangles in the translucent list. Of the GS's tests:
   the alpha test as blending (a cut-out's transparent texels have alpha 0) or not drawn (never passes); the
   scissor by clipping the triangles here; the destination alpha test left out (see below). */
static int sSecAlpha; /* this render has written the GS's alpha alone into the secondary buffer (see below) */
static int sHdrLive;  /* the polygon header the TA has now is GsPvr_Direct's last (sDirLast.hdr): nothing sent since */

/* Whether draw d is one the PVR draws (into whichever buffer). */
static int draw_kept(const GsDraw *d, uint32_t n) {
    uint32_t key;
    if (d->native || d->pipeline < 0 || d->pipeline >= sPipeCount) { return 0; }
    {   /* BT3_PVR_SKIP=<lo>-<hi>: those draws left out (finding a wrong one) */
        static int read;
        static unsigned lo = 1, hi = 0;
        if (!read) {
            read = 1;
            if (getenv("BT3_PVR_SKIP") == NULL || sscanf(getenv("BT3_PVR_SKIP"), "%u-%u", &lo, &hi) != 2) { lo = 1; hi = 0; }
        }
        if (n >= lo && n <= hi) { return 0; }
    }
    key = sPipeKey[d->pipeline];
    if (((key >> 10) & 3) == 0 || ((key >> 22) & 1)) { return 0; } /* depth test NEVER; a table pass */
    if (((key >> 13) & 3) != 0) { return 0; }  /* lines, points */
    if ((key >> 16 & 0xF) == 0) { return 0; }  /* writes no colour and no alpha (depth only) */
    if (d->mode[3] == 1) { return 0; }          /* alpha test NEVER (with KEEP: nothing is written) */
    /* The destination alpha test (bit 7 of the alpha already there; the PVR cannot test it). The stage's
       layers drawn by vertex programs (the grass on a fight's ground) are drawn without it: the alpha is set
       to 0 before them, and each layer sets bit 7 where it draws (FBA), so the test only keeps a second layer
       from blending over a pixel a first one has; without it overlapping grass is a little thicker. The 2D
       ones (a HUD bar's fill cut to length by a mask) are left out: the whole fill would show. */
    if (d->misc[1] != 0.0f && !d->vu && !d->vurec) { return 0; }
    return 1;
}

#if GS_STRIPS
/* A corner of gsVerts as draw_one's triangles have it; 1 = inside the scissor */
static inline int strip_corner(const Vtx *v, const PvrTex *t, ClipV *o, float x0, float y0, float x1, float y1) {
    float r = 1.0f / v->q; /* (never 0: gs_draw.c strip_vtx) */
    o->x = v->x * sSx;
    o->y = v->y * sSy;
    o->z = PVR_Z(clampf(v->z, 0.0f, 1.0f));
    o->u = t != NULL ? v->s * r * sUs : 0.0f;
    o->v = t != NULL ? v->t * r * sVs : 0.0f;
    o->c[0] = (float)gs_unit(v->r);
    o->c[1] = (float)gs_unit(v->g);
    o->c[2] = (float)gs_unit(v->b);
    o->c[3] = sFixA >= 0.0f ? sFixA * 255.0f : (float)gs_unit(v->a);
    return scissor_inside(o->x, o->y, x0, y0, x1, y1);
}

/* A strip draw (gs_draw.c GsGpu_Strip): its strips as the PVR's, a triangle with a corner outside the scissor
   clipped and sent alone (emit_poly), the strip going on after it */
static void strip_draw(const GsDraw *d, const PvrTex *t, float x0, float y0, float x1, float y1) {
    uint32_t i = d->first, e = d->first + d->count;
    while (i < e) {
        uint32_t a = i, j;
        ClipV w[3];
        int in[3], on = 0;
        for (i++; i < e && !gsVertRun[i]; i++) {
        }
        if (i - a < 3) {
            continue;
        }
        /* (the window of three corners a ring: w[r0], w[r1], w[r2] the triangle's in order) */
        int r0 = 0, r1 = 1, r2 = 2;
        in[0] = strip_corner(&gsVerts[a], t, &w[0], x0, y0, x1, y1);
        in[1] = strip_corner(&gsVerts[a + 1], t, &w[1], x0, y0, x1, y1);
        for (j = a + 2; j < i; j++) {
            in[r2] = strip_corner(&gsVerts[j], t, &w[r2], x0, y0, x1, y1);
            if (in[0] && in[1] && in[2]) {
                if (on && sStrip.n > 0) {
                    if (sSent + sizeof(pvr_vertex_t) <= SCENE_BUDGET) {
                        strip_add(&w[r2]);
                        sSent += sizeof(pvr_vertex_t);
                        sTris++;
                    } else {
                        sDropped++;
                        strip_end();
                        on = 0;
                    }
                } else {
                    strip_end();
                    if (sSent + 3 * sizeof(pvr_vertex_t) <= SCENE_BUDGET) {
                        strip_add(&w[r0]);
                        strip_add(&w[r1]);
                        strip_add(&w[r2]);
                        sSent += 3 * sizeof(pvr_vertex_t);
                        sTris++;
                        on = 1;
                    } else {
                        sDropped++;
                    }
                }
            } else {
                ClipV c[12];
                strip_end();
                on = 0;
                c[0] = w[r0];
                c[1] = w[r1];
                c[2] = w[r2];
                emit_poly(c, 3, x0, y0, x1, y1);
            }
            r0 = r1;
            r1 = r2;
            r2 = 3 - r0 - r1;
        }
        strip_end();
    }
}
#endif

/* draw_one's polygons for draw d: their contexts (pass[0..n), n returned; 0 = nothing drawn) and *tp the texture as
   they draw it (NULL: none); sUs, sVs, sFixA and the tint (sTintCur) the corners are made by */
static int draw_head(const GsDraw *d, const PvrTex **tp, pvr_poly_cxt_t pass[2]) {
    const PvrTex *t = *tp;
    uint32_t key = sPipeKey[d->pipeline], bkey, k;
    pvr_poly_cxt_t cxt;
    int np, tfx;

    static int noTex = -1;
    if (noTex < 0) { noTex = getenv("BT3_PVR_NOTEX") != NULL; }
    if (d->vu && noTex) { t = NULL; } /* 3D in its colours only */
    sUs = sVs = 1.0f;
    if (t != NULL && t->mem != NULL) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, (int)t->fmt, (int)t->pw, (int)t->ph, t->base,
                         (d->sampler & 1) ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        cxt.txr.uv_clamp = (pvr_uv_clamp_t)(((d->sampler & 2) ? PVR_UVCLAMP_U : 0) | ((d->sampler & 4) ? PVR_UVCLAMP_V : 0));
        tfx = d->mode[1];
        cxt.txr.env = tfx == 1 ? (d->mode[2] ? PVR_TXRENV_REPLACE : PVR_TXRENV_DECAL) : PVR_TXRENV_MODULATEALPHA;
        if (d->mode[0] == 2 && !d->mode[2]) { cxt.txr.alpha = PVR_TXRALPHA_DISABLE; } /* (a frame buffer without its alpha: the corners') */
        if (d->mode[0] == 2) {
            sUs = (float)GS_W / (float)t->w;
            sVs = (float)GS_H / (float)t->h;
        }
    } else {
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        t = NULL;
    }
    sTintCur = NULL;
    sTintFlat = 0;
    if (t != NULL && d->tint != 0) {
        for (k = 0; k < 3; k++) {
            sTintNow.k[k] = (float)((d->tint >> (k * 8)) & 0xFF) / 255.0f;
            sTintNow.o[k] = (float)((d->tint >> (24 + k * 8)) & 0xFF);
        }
        sTintNow.alpha = ((d->tint >> 56) & 1) ? (int)((d->tint >> 48) & 0xFF) : -1;
        sTintCur = &sTintNow;
        sTintDraws++;
    }
    if (sTintCur != NULL) { /* (a tinted palette: above) */
        cxt.gen.specular = PVR_SPECULAR_ENABLE;
        if (cxt.txr.env != PVR_TXRENV_MODULATEALPHA) { cxt.txr.env = PVR_TXRENV_MODULATEALPHA; sTintFlat = 1; }
        if (sTintCur->alpha >= 0) { cxt.txr.alpha = PVR_TXRALPHA_DISABLE; }
    }
    bkey = key & 0x1FF;
    cxt.gen.culling = PVR_CULLING_NONE;
    {   /* the GS's depth test (bigger is nearer, as the PVR's) and depth write, 2D and 3D alike */
        int ztst = (int)((key >> 10) & 3);
        cxt.depth.comparison = ztst == 1 ? PVR_DEPTHCMP_ALWAYS : ztst == 2 ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_GREATER;
        cxt.depth.write = ((key >> 12) & 1) ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE; /* (KOS's: 0 = write) */
    }
    sFixA = -1.0f;
    if (bkey & 1) {
        int c = (int)((bkey >> 7) & 3), ks = (int)((bkey >> 1) & 7), kd = (int)((bkey >> 4) & 7);
        cxt.blend.src = (pvr_blend_mode_t)blend_factor(ks, c, 0);
        cxt.blend.dst = (pvr_blend_mode_t)blend_factor(kd, c, 1);
        if (c == 2 && ks == 3 && (kd == 1 || kd == 5) && d->blendc >= 0.99f) {
            /* The destination less the source, by 1 (the fighters' shading ramp darkens what is drawn): the PVR
               cannot subtract, so the destination times one minus the source, its nearest. The same where the
               destination is white, less dark where it is dark already; left out, the fighters had no shading. */
            cxt.blend.src = PVR_BLEND_ZERO;
            cxt.blend.dst = PVR_BLEND_INVDESTCOLOR; /* (as the destination's factor: one minus the source colour) */
        }
        if (c == 2 && (ks == 2 || ks == 4 || kd == 2 || kd == 4)) {
            /* The constant factor (the blur steps between the work buffers mix two by it, half and half) as the
               source alpha: the vertices' alpha is the constant, the texture's alpha is not used (DECAL instead of
               REPLACE, which would take it). The alpha written is then blended too, where the GS writes the
               source's. */
            sFixA = d->blendc > 1.0f ? 1.0f : d->blendc;
            if (ks == 2 || ks == 4) { cxt.blend.src = ks == 2 ? PVR_BLEND_SRCALPHA : PVR_BLEND_INVSRCALPHA; }
            if (kd == 2 || kd == 4) { cxt.blend.dst = kd == 2 ? PVR_BLEND_SRCALPHA : PVR_BLEND_INVSRCALPHA; }
            if (t != NULL) {
                cxt.txr.alpha = PVR_TXRALPHA_DISABLE;
                if (cxt.txr.env == PVR_TXRENV_REPLACE) { cxt.txr.env = PVR_TXRENV_DECAL; }
            }
        }
    } else if (d->mode[3] >= 6 && d->mode[3] <= 7) {
        /* alpha test GEQUAL / GREATER with blending off: a cut-out. Blending by the source alpha leaves its
           transparent texels (alpha 0) out, as the test does; partly transparent ones come out soft. */
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    } else {
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
    }
    /* The GS's alpha written alone (FRAME.FBMSK: the colour masked) is kept in the PVR's secondary
       accumulation buffer, which starts each tile at 0: the PVR cannot write one channel of a pixel. A
       blend after it by the destination's alpha takes that alpha (`sSecAlpha`), in two polygons: the source
       times the stored alpha into the secondary buffer, then that added to the picture, which keeps one minus
       it. (A story chapter's picture is revealed by a moving mask so: the alpha cleared, the mask's alpha
       written, the picture blended by it. Without it the picture was blended by whatever alpha was there,
       and showed as noise.) The second polygon takes the stored alpha times the source's, so it is exact for
       an opaque source. */
    pass[0] = cxt;
    np = 1;
    if ((key >> 16 & 15) == 0) { /* nothing written but the depth (ObjDraw_SetLight's depth pass): the picture kept */
        if (!((key >> 12) & 1)) {
            return 0;
        }
        pass[0].blend.src = PVR_BLEND_ZERO;
        pass[0].blend.dst = PVR_BLEND_ONE;
    } else if ((key >> 16 & 7) == 0) { /* the alpha alone */
        pass[0].blend.src = PVR_BLEND_ONE;
        pass[0].blend.dst = PVR_BLEND_ZERO;
        pass[0].blend.dst_enable = PVR_BLEND_ENABLE;
        sSecAlpha = 1;
    } else if ((bkey & 1) && ((bkey >> 7) & 3) == 1 && sSecAlpha) {
        pass[0].blend.dst = PVR_BLEND_ZERO;
        pass[0].blend.dst_enable = PVR_BLEND_ENABLE;
        pass[1] = cxt;
        pass[1].blend.src_enable = PVR_BLEND_ENABLE;
        pass[1].blend.src = PVR_BLEND_ONE;
        pass[1].blend.dst = cxt.blend.dst == PVR_BLEND_INVDESTALPHA ? PVR_BLEND_INVSRCALPHA :
                            cxt.blend.dst == PVR_BLEND_DESTALPHA ? PVR_BLEND_SRCALPHA : cxt.blend.dst;
        np = 2;
    }
    *tp = t;
    return np;
}

/* Draw n into the render being built; `t` its texture (a frame buffer's: the version it samples), or NULL. */
static void draw_one(uint32_t n, const PvrTex *t) {
    const GsDraw *d = &gsDraws[n];
    uint32_t key = sPipeKey[d->pipeline], k;
    pvr_poly_cxt_t pass[2];
    pvr_poly_hdr_t hdr;
    int np, p;

    if ((np = draw_head(d, &t, pass)) == 0) {
        return;
    }
    for (p = 0; p < np; p++) {
        pvr_poly_compile(&hdr, &pass[p]);
        if (sSent + sizeof(hdr) + 3 * sizeof(pvr_vertex_t) > SCENE_BUDGET) {
            sDropped += d->count / 3;
            break;
        }
        sSent += sizeof(hdr);
        pvr_prim(&hdr, sizeof(hdr));
        sHdrLive = 0;
        /* the scissor (x, y, width, height at SCALE) in the render's pixels */
        float x0 = (float)d->scissor[0] / (float)SCALE * sSx, y0 = (float)d->scissor[1] / (float)SCALE * sSy;
        float x1 = (float)(d->scissor[0] + d->scissor[2]) / (float)SCALE * sSx, y1 = (float)(d->scissor[1] + d->scissor[3]) / (float)SCALE * sSy;
        if (x1 > sRw) { x1 = sRw; }
        if (y1 > sRh) { y1 = sRh; }
        if (d->vu) {
            vu_draw((int)((key >> 20) & 3), &gsVuUni[d->uniform], &gsVuIdx[d->first], d->count, x0, y0, x1, y1);
            continue;
        }
#if GS_STRIPS
        if (d->strip) {
            strip_draw(d, t, x0, y0, x1, y1);
            continue;
        }
#endif
        for (k = 0; k + 2 < d->count; k += 3) {
            ClipV a[12];
            int j, inside = 1;
            for (j = 0; j < 3; j++) {
                const Vtx *v = &gsVerts[d->first + k + j];
                float q = v->q != 0.0f ? v->q : 1.0f;
                a[j].x = v->x * sSx;
                a[j].y = v->y * sSy;
                a[j].z = PVR_Z(clampf(v->z, 0.0f, 1.0f));
                a[j].u = t != NULL ? v->s / q * sUs : 0.0f;
                a[j].v = t != NULL ? v->t / q * sVs : 0.0f;
                a[j].c[0] = (float)gs_unit(v->r);
                a[j].c[1] = (float)gs_unit(v->g);
                a[j].c[2] = (float)gs_unit(v->b);
                a[j].c[3] = sFixA >= 0.0f ? sFixA * 255.0f : (float)gs_unit(v->a);
                if (!scissor_inside(a[j].x, a[j].y, x0, y0, x1, y1)) { inside = 0; }
            }
            /* A triangle that needs no clipping and starts with the last two corners of the one before (a strip
               the game drew, handed on as triangles: the records of the vertex programs) continues the PVR's
               strip, one vertex instead of three. */
            if (inside && sStrip.n > 0 && k >= 3 && !memcmp(&gsVerts[d->first + k], &gsVerts[d->first + k - 2], sizeof(Vtx)) &&
                !memcmp(&gsVerts[d->first + k + 1], &gsVerts[d->first + k - 1], sizeof(Vtx))) {
                if (sSent + sizeof(pvr_vertex_t) <= SCENE_BUDGET) {
                    strip_add(&a[2]);
                    sSent += sizeof(pvr_vertex_t);
                    sTris++;
                } else {
                    sDropped++;
                }
                continue;
            }
            strip_end();
            if (inside && sSent + 3 * sizeof(pvr_vertex_t) <= SCENE_BUDGET) {
                strip_add(&a[0]);
                strip_add(&a[1]);
                strip_add(&a[2]);
                sSent += 3 * sizeof(pvr_vertex_t);
                sTris++;
                continue;
            }
            emit_poly(a, 3, x0, y0, x1, y1);
        }
        strip_end();
    }
}

/* ---- the other frame buffers: textures the PVR renders into -------------------------------------------
   The game builds its screen effects by drawing one frame buffer into another: the glare and the object glow
   (the picture shrunk into a work buffer, down to 64 x 64, blurred between two buffers, added back), the blur of
   distant things, the fighters' shadow (drawn into a buffer, laid on the ground by a vertex program). Here every
   buffer other than the shown one is a texture in PVR memory (ARGB4444: the glow is made from the picture's
   alpha), and each run of draws into one is a render to that texture. A run starts by drawing what the buffer
   held as its background (a render starts from a cleared tile buffer).

   The shown buffer is drawn once, at the end, in one render: its depth (the stage that hides the fighters' feet)
   cannot be carried from one render to the next. When a draw samples the shown buffer partway through (the
   copies the effects start from), the shown buffer's draws so far are rendered into a texture as well, the
   capture, which goes on from the previous capture of the same frame (without depth: the capture is the effects'
   source, blurred or mixed back).

   A buffer's texture is a version: a draw of the final render samples the one there was when the game drew it,
   so a buffer drawn into again after that gets a new one (the work buffer of the glare is the next effect's too).
   The superseded versions are freed when the frame is done; a buffer's last one is kept for the next frame. */
typedef struct TgVer {
    PvrTex t;          /* w x h (powers of two, at least 32), ARGB4444, not twiddled */
    int final;         /* sampled by a draw of the final render: kept as it is until the frame is done */
    int alphaBad;      /* made from an alpha the PVR could not make (below) */
} TgVer;

static TgVer *sTgVer[MAX_TARGETS];     /* each buffer's version now (NULL: never drawn) */
static TgVer *sCap;                    /* the shown buffer's capture, 512 x 512 */
static int sCapValid;                  /* the capture holds this frame's shown draws up to sShownCapped */
static TgVer *sOldVer[256];            /* superseded this frame, freed at its end */
static int sOldCount;
static const PvrTex **sDrawSrc;        /* per draw of the frame: the version it samples */
static uint32_t *sShownList, sShownCount, sShownCapped, sListCap;
static uint32_t *sRun, sRunCount;      /* the run being collected (draws into sRunTarget) */
static int sRunTarget = -1;
static uint32_t sRunW, sRunH;          /* the run's extent in GS pixels */
static int sRunBad;                    /* the run samples a version with alphaBad */
static unsigned sRenders;              /* this frame */
/* The screen's render open since the lists were full in the middle of the frame (spill): the draws sent so far,
   for the shown buffer sStreamShown; sStreamTA the table passes' state (frame_render) after them. */
static int sStreamOpen, sStreamShown, sStreamTA;
static unsigned sSpills;               /* this frame (BT3_GS_VERBOSE) */

static TgVer *ver_new(uint32_t w, uint32_t h) {
    TgVer *v = calloc(1, sizeof(TgVer));
    if (v == NULL) { return NULL; }
    v->t.w = w;
    v->t.h = h;
    v->t.bytes = w * h * 2;
    v->t.mem = pvr_alloc(v->t.bytes, "a render target", 0);
    if (v->t.mem == NULL) {
        free(v);
        return NULL;
    }
    v->t.pw = w;
    v->t.ph = h;
    v->t.fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    v->t.base = v->t.mem;
    v->t.format = GS_TEXFMT_RGBA8;
    return v;
}

static void ver_free(TgVer *v) {
    if (v == NULL) { return; }
    pvr_mem_free(v->t.mem);
    free(v);
}

static void ver_retire(TgVer *v) {
    if (v == NULL) { return; }
    if (sOldCount < (int)(sizeof(sOldVer) / sizeof(sOldVer[0]))) { sOldVer[sOldCount++] = v; } else { ver_free(v); }
}

static uint32_t pow2_rt(uint32_t n) {
    uint32_t p = 32;
    while (p < n && p < 1024) { p <<= 1; }
    return p;
}

/* A render begun: into `dst` (NULL: the screen), over `bg` drawn first (NULL: cleared). */
extern uint64_t gGpuWaitNs; /* gs_draw.c: waiting for the PVR's last render to end (pvr_wait_ready) */
static void scene_begin(TgVer *dst, const TgVer *bg) {
    extern uint64_t Gs_NowNs(void);
    uint64_t t0 = Gs_NowNs();
    pvr_wait_ready();
    gGpuWaitNs += Gs_NowNs() - t0;
    sSent = 0;
    sSecAlpha = 0;
    sHdrLive = 0;
    if (dst != NULL) {
        rtt_argb4444();
        pvr_scene_begin_rtt(dst->t.mem, dst->t.w, dst->t.h, dst->t.w);
        sSx = sSy = 1.0f;
        sRw = (float)dst->t.w;
        sRh = (float)dst->t.h;
    } else {
#ifdef __DREAMCAST__
        PVR_SET(PVR_FB_CFG_2, sFbCfg2);
#endif
        pvr_scene_begin();
        sSx = 640.0f / 512.0f;
        sSy = 480.0f / 448.0f;
        sRw = 640.0f;
        sRh = 480.0f;
    }
    pvr_list_begin(PVR_LIST_TR_POLY);
    if (bg != NULL) { /* what the buffer held: its texture 1:1, every texel as it was (alpha too) */
        pvr_poly_cxt_t cxt;
        pvr_poly_hdr_t hdr;
        ClipV a[4];
        int j;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, (int)bg->t.fmt, (int)bg->t.pw, (int)bg->t.ph, bg->t.base, PVR_FILTER_NEAREST);
        cxt.txr.env = PVR_TXRENV_REPLACE;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
        pvr_poly_compile(&hdr, &cxt);
        pvr_prim(&hdr, sizeof(hdr));
        sSent += sizeof(hdr);
        for (j = 0; j < 4; j++) {
            a[j].x = (j == 1 || j == 2) ? (float)bg->t.w * sSx : 0.0f;
            a[j].y = j >= 2 ? (float)bg->t.h * sSy : 0.0f;
            a[j].z = PVR_Z(0.0f);
            a[j].u = (j == 1 || j == 2) ? 1.0f : 0.0f;
            a[j].v = j >= 2 ? 1.0f : 0.0f;
            a[j].c[0] = a[j].c[1] = a[j].c[2] = a[j].c[3] = 255.0f;
        }
        emit_poly(a, 4, 0.0f, 0.0f, sRw, sRh);
    }
}

/* One render of draws `idx` (n of them): into `dst` (NULL: the screen), over `bg` drawn first (NULL: cleared). */
static void render_run(const uint32_t *idx, uint32_t n, TgVer *dst, const TgVer *bg) {
    uint32_t i;

    if (getenv("BT3_PVR_LOG") != NULL && (int)gGsFrame == atoi(getenv("BT3_PVR_LOG"))) {
        fprintf(stderr, "pvr: render %u: %u draws (%u..%u) into %s %ux%u%s\n", sRenders, n, n ? idx[0] : 0, n ? idx[n - 1] : 0,
                dst == NULL ? "the screen" : dst == sCap ? "the capture" : "a buffer", dst ? dst->t.w : 640, dst ? dst->t.h : 480, bg ? " over what it held" : "");
    }
    if (dst != NULL || !sStreamOpen) { scene_begin(dst, bg); } /* (else the screen's, open since a spill) */
    if (dst == NULL) { sStreamOpen = 0; }
    for (i = 0; i < n; i++) { draw_one(idx[i], sDrawSrc[idx[i]]); }
    sFixA = -1.0f;
    pvr_list_finish();
    pvr_scene_finish(); /* renders (the screen's: gpu_pvr shows the picture, kos_pvr_present) */
    sRenders++;
    if (dst != NULL && getenv("BT3_PVR_RTDUMP") != NULL && (int)gGsFrame == atoi(getenv("BT3_PVR_RTDUMP"))) {
        /* BT3_PVR_RTDUMP=<frame>: each render to a texture of that frame as rt_NN_<first draw>.ppm (colour, then
           alpha beside it) in BT3_PVR_RTDIR (default .) */
        char name[512];
        FILE *fp;
        const char *dir = getenv("BT3_PVR_RTDIR") != NULL ? getenv("BT3_PVR_RTDIR") : ".";
        snprintf(name, sizeof(name), "%s/rt_%02u_%u.ppm", dir, sRenders, n ? idx[0] : 0);
        if ((fp = fopen(name, "wb")) != NULL) {
            const uint16_t *px = (const uint16_t *)dst->t.mem;
            uint32_t x, y;
            fprintf(fp, "P6\n%u %u\n255\n", dst->t.w * 2, dst->t.h);
            for (y = 0; y < dst->t.h; y++) {
                for (x = 0; x < dst->t.w * 2; x++) {
                    uint16_t c = px[y * dst->t.w + (x % dst->t.w)];
                    uint8_t rgb[3];
                    if (x < dst->t.w) { rgb[0] = (uint8_t)(((c >> 8) & 15) * 17); rgb[1] = (uint8_t)(((c >> 4) & 15) * 17); rgb[2] = (uint8_t)((c & 15) * 17); }
                    else { rgb[0] = rgb[1] = rgb[2] = (uint8_t)((c >> 12) * 17); }
                    fwrite(rgb, 1, 3, fp);
                }
            }
            fclose(fp);
        }
    }
    {
        static uint32_t peak;
        if (sSent > peak) { peak = sSent; }
        if (sDropped != 0) {
            static unsigned told;
            if (getenv("BT3_GS_VERBOSE") != NULL || told++ < 10) {
                fprintf(stderr, "pvr: frame %u: a render of %u triangles left some out (%u KB, the most so far %u KB of %u): the vertex buffer is full\n",
                        gGsFrame, sTris, sSent >> 10, peak >> 10, (unsigned)(SCENE_BUDGET >> 10));
            }
            sDropped = 0;
        }
    }
}

/* BT3_PVR_NO_RTT=1: nothing rendered to a texture (the draws into the other frame buffers and those that sample
   one left out). The Dreamcast's default while its 8 MB of PVR memory has no room for them (gpu_pvr has 16 for
   now: gpu_pvr/kos/arch/arch.h GPU_PVR_VRAM_MB). */
static int no_rtt(void) {
#ifdef BT3_NO_RTT
    return 1; /* (the build's: Makefile.dc DC_RTT) */
#else
    return !Rnd_FxOn(); /* (port/src/rnd_fx.c: the effects as records ask the same) */
#endif
}

/* The run collected for buffer sRunTarget, rendered into its texture. */
static void run_flush(void) {
    int tg = sRunTarget;
    TgVer *cur, *dst;
    uint32_t w, h;

    if (tg < 0) { return; }
    sRunTarget = -1;
    if (no_rtt()) { sRunCount = 0; return; }
    cur = sTgVer[tg];
    if (sRunBad) { /* left out: what it would make is wrong (the buffer is marked so its readers are left out too) */
        if (cur == NULL && (cur = sTgVer[tg] = ver_new(32, 32)) == NULL) { sRunCount = 0; return; }
        cur->alphaBad = 1;
        gsTargets[tg].cleared = 1;
        sRunCount = 0;
        return;
    }
    w = pow2_rt(sRunW);
    h = pow2_rt(sRunH);
    if (cur != NULL) {
        if (cur->t.w > w) { w = cur->t.w; }
        if (cur->t.h > h) { h = cur->t.h; }
    }
    if (cur != NULL && !cur->final && cur->t.w == w && cur->t.h == h) {
        dst = cur; /* drawn on in place: each pixel's background is read before its tile is written */
    } else {
        dst = ver_new(w, h);
        if (dst == NULL) {
            static int told;
            if (!told++) { fprintf(stderr, "pvr: no PVR memory for a %ux%u frame buffer: its effects are left out\n", w, h); }
            sRunCount = 0;
            return;
        }
        sTgVer[tg] = dst;
    }
    render_run(sRun, sRunCount, dst, cur);
    dst->alphaBad = 0;
    if (cur != NULL && cur != dst) { ver_retire(cur); }
    gsTargets[tg].cleared = 1;
    sRunCount = 0;
}

/* The shown buffer's draws so far into the capture (going on from the last one of this frame). */
static void capture_update(void) {
    if (sCapValid && sShownCapped == sShownCount) { return; }
    if (sCap == NULL && (sCap = ver_new(512, 512)) == NULL) { return; }
    if (sCap->final) { /* (the shown buffer's capture is only read by runs, never by the final render) */
        return;
    }
    render_run(sShownList + sShownCapped, sShownCount - sShownCapped, sCap, sCapValid ? sCap : NULL);
    sShownCapped = sShownCount;
    sCapValid = 1;
}

static void target_drop(int i) {
    if (sTgVer[i] != NULL && !sTgVer[i]->final) { ver_free(sTgVer[i]); } else { ver_retire(sTgVer[i]); }
    sTgVer[i] = NULL;
}

/* The shown buffer: the one sceGsSwapDBuff set up for this frame, else the first target (as gs_gl.c). */
static int shown_target(void) {
    int shown = gGsMainFbp >= 0 ? GsDraw_TargetGet((uint32_t)gGsMainFbp, 0) : -1;
    if (shown < 0 && gsTargetCount > 0) { shown = 0; }
    return shown;
}

/* What of a frame's draws goes on is decided at the shown buffer the first spill found; a frame that changes it
   after (the buffers swapped in between) has drawn its first part for another. */
static void stream_moved(int shown) {
    if (sStreamOpen && shown != sStreamShown) {
        Port_Lost("the shown buffer changed after the PVR had drawn part of the frame (gs_pvr.c spill)", (uint32_t)shown, __builtin_return_address(0));
    }
}

/* The lists full in the middle of a frame (gs_draw.c lists_room): without renders to textures every draw kept goes
   to the screen's render in the order made, so they are drawn now, the render begun if it is not (it is finished at
   the frame's end, frame_render). What is left out is as frame_render leaves it out. With renders to textures the
   frame is needed whole: not taken. */
static int spill(void) {
    uint32_t n;
    int shown;

    if (!no_rtt()) { return 0; }
    shown = shown_target();
    stream_moved(shown);
    if (!sStreamOpen) {
        scene_begin(NULL, NULL);
        sStreamOpen = 1;
        sStreamShown = shown;
        sStreamTA = 0;
    }
    for (n = 0; n < gsDrawCount; n++) {
        const GsDraw *d = &gsDraws[n];
        uint32_t key;
        if (d->native == 3 && d->target == sStreamShown && d->pipeline >= 0 && d->pipeline < sPipeCount && (sPipeKey[d->pipeline] >> 16 & 8)) { sStreamTA = 1; }
        if (!draw_kept(d, n) || d->mode[0] == 2 || d->target != sStreamShown) { continue; }
        key = sPipeKey[d->pipeline];
        if ((key >> 16 & 7) == 0) { sStreamTA = 0; }
        if (sStreamTA && (key & 1) && ((key >> 7) & 3) == 1) { continue; }
        draw_one(n, d->mode[0] != 0 ? (const PvrTex *)d->tex : NULL);
    }
    sSpills++;
    return 1;
}

#if GS_STRIPS
/* A vertex program's strip sent straight to the screen's render (gs_draw.c GsGpu_Direct, include/port/rnd.h
   Rnd_Direct): what is waiting in the lists sent first (spill), so the order is as made; then the strip's polygon,
   its corners after it as the caller makes them (Port_DirectSend). Only what draw_one sends as one polygon over the
   whole screen without a tint (else 0: the strip goes through the lists as before); 2 = nothing drawn, as
   spill and draw_one leave it out. */
static inline int same_words(const void *a, const void *b, unsigned bytes) {
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    unsigned i;
    for (i = 0; i < bytes / 4; i++) {
        if (x[i] != y[i]) { return 0; }
    }
    return 1;
}

/* The last direct strip's polygon (draw_head's, and its header compiled), kept: a model's next strip almost always
   draws as the one before. draw_head reads only the draw, its texture's record and sSecAlpha; the same of those,
   the same polygon, and then no new header either while the TA still has it (sHdrLive). */
static struct {
    int ok, sec, secAfter;
    GsDraw d;
    const PvrTex *tp, *tout; /* the texture given, the one drawn (NULL: none) */
    PvrTex t;                /* *tp as it was (a texture moved, or another in its record, is another polygon) */
    float us, vs, fixa;
    pvr_poly_hdr_t hdr;
} sDirLast;

int GsPvr_Direct(RndDirect *o, const GsDraw *d, uint32_t tris, uint32_t verts) {
    const PvrTex *t = d->mode[0] != 0 ? (const PvrTex *)d->tex : NULL;
    pvr_poly_cxt_t pass[2];
    pvr_poly_hdr_t *hdr = &sDirLast.hdr;
    uint32_t key, hdrBytes;
    float x0, y0, x1, y1;
    int np;

    if (!no_rtt() || d->mode[0] == 2) {
        return 0;
    }
    if (!draw_kept(d, 0xFFFFFFFFu)) {
        return 2;
    }
    spill();
    gsVertCount = 0;
    gsDrawCount = 0;
    if (d->target != sStreamShown) {
        return 2;
    }
    key = sPipeKey[d->pipeline];
    if ((key >> 16 & 7) == 0) { sStreamTA = 0; }
    if (sStreamTA && (key & 1) && ((key >> 7) & 3) == 1) {
        return 2;
    }
    x0 = (float)d->scissor[0] / (float)SCALE * sSx;
    y0 = (float)d->scissor[1] / (float)SCALE * sSy;
    x1 = (float)(d->scissor[0] + d->scissor[2]) / (float)SCALE * sSx;
    y1 = (float)(d->scissor[1] + d->scissor[3]) / (float)SCALE * sSy;
    if (x0 > 0.0f || y0 > 0.0f || x1 < sRw || y1 < sRh) {
        return 0; /* (a scissor inside the screen: draw_one clips to it) */
    }
    if (sDirLast.ok && sDirLast.sec == sSecAlpha && sDirLast.tp == t && (t == NULL || same_words(&sDirLast.t, t, sizeof(*t))) &&
        same_words(&sDirLast.d, d, sizeof(*d))) {
        /* (draw_head's results as it left them) */
        t = sDirLast.tout;
        sUs = sDirLast.us;
        sVs = sDirLast.vs;
        sFixA = sDirLast.fixa;
        sTintCur = NULL;
        sTintFlat = 0;
        sSecAlpha = sDirLast.secAfter;
    } else {
        const PvrTex *given = t;
        int sec = sSecAlpha;
        sDirLast.ok = 0;
        sHdrLive = 0;
        if ((np = draw_head(d, &t, pass)) == 0) {
            return 2;
        }
        if (np != 1 || sTintCur != NULL) {
            return 0;
        }
        pvr_poly_compile(hdr, &pass[0]);
        sDirLast.ok = 1;
        sDirLast.sec = sec;
        sDirLast.secAfter = sSecAlpha;
        sDirLast.d = *d;
        sDirLast.tp = given;
        if (given != NULL) { sDirLast.t = *given; }
        sDirLast.tout = t;
        sDirLast.us = sUs;
        sDirLast.vs = sVs;
        sDirLast.fixa = sFixA;
    }
    strip_end();
    hdrBytes = sHdrLive ? 0 : sizeof(*hdr);
    if (sSent + hdrBytes + verts * sizeof(pvr_vertex_t) > SCENE_BUDGET) {
        sDropped += tris;
        return 2;
    }
    if (!sHdrLive) {
        pvr_prim(hdr, sizeof(*hdr));
        sHdrLive = 1;
    }
    sSent += hdrBytes + verts * sizeof(pvr_vertex_t);
    sTris += tris;
    o->sx = sSx;
    o->sy = sSy;
    o->tex = t != NULL;
    o->us = t != NULL ? sUs : 0.0f;
    o->vs = t != NULL ? sVs : 0.0f;
    o->fixa = sFixA >= 0.0f ? (int)(sFixA * 255.0f + 0.5f) : -1;
    return 1;
}

/* The corners of a strip GsPvr_Direct took: each run of triangles not left out (adc) as one of the PVR's strips */
void Port_DirectSend(const RndPvrVtx *v, const uint8_t *adc, uint32_t n) {
    uint32_t k = 2, e, i;
    while (k < n) {
        if (adc[k]) {
            k++;
            continue;
        }
        for (e = k + 1; e < n && !adc[e]; e++) {
        }
        for (i = k - 2; i < e; i++) {
            pvr_vertex_t *pv = (pvr_vertex_t *)pvr_dr_target();
            pv->flags = i + 1 == e ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            pv->x = v[i].x;
            pv->y = v[i].y;
            pv->z = v[i].z;
            pv->u = v[i].u;
            pv->v = v[i].v;
            pv->argb = v[i].argb;
            pv->oargb = 0;
            pvr_dr_commit(pv);
        }
        k = e;
    }
}
#endif

static void frame_render(int shown) {
    uint32_t n;
    int i, tableAlpha = sStreamOpen ? sStreamTA : 0; /* (after a spill: as its draws left it) */

    if (sListCap < gsDrawCount) {
        sListCap = gsDrawCount + 1024;
        sDrawSrc = realloc(sDrawSrc, sListCap * sizeof(*sDrawSrc));
        sShownList = realloc(sShownList, sListCap * sizeof(*sShownList));
        sRun = realloc(sRun, sListCap * sizeof(*sRun));
        if (sDrawSrc == NULL || sShownList == NULL || sRun == NULL) { abort(); }
    }
    stream_moved(shown);
    sShownCount = sShownCapped = sRunCount = 0;
    sCapValid = 0;
    sRunTarget = -1;
    sRenders = 0;
    sTris = 0;
    for (n = 0; n < gsDrawCount; n++) {
        const GsDraw *d = &gsDraws[n];
        const PvrTex *src = d->mode[0] != 0 ? (const PvrTex *)d->tex : NULL;

        /* The table passes (native 3, gs_draw.c depth_clut) put a byte of the depth through a table into the
           shown buffer's alpha: what glares (the far sky) and how much the blur of distant things is mixed in. The
           PVR cannot read its depth, so from such a pass until the alpha is written anew the shown buffer's alpha
           is not the game's, and what is made from it is left out: the copies of the picture taken by it, the
           work buffers made from those, and their drawing back (marked alphaBad along the way). */
        if (d->native == 3 && d->target == shown && d->pipeline >= 0 && d->pipeline < sPipeCount && (sPipeKey[d->pipeline] >> 16 & 8)) { tableAlpha = 1; }
        if (!draw_kept(d, n)) { continue; }
        if (no_rtt() && (d->mode[0] == 2 || d->target != shown)) { continue; }
        if (d->target == shown) {
            uint32_t key = sPipeKey[d->pipeline];
            if ((key >> 16 & 7) == 0) { tableAlpha = 0; } /* the alpha written anew (the secondary buffer, draw_one) */
            if (tableAlpha && (key & 1) && ((key >> 7) & 3) == 1) { continue; } /* blended by that alpha */
        }
        if (d->mode[0] == 2) { /* a frame buffer as its texture: the version of it there is now */
            int s = (int)((const PvrTex *)d->tex - sTgCol);
            TgVer *v;
            if (s < 0 || s >= MAX_TARGETS) { continue; }
            if (s == shown) {
                static TgVer bad = {{0}, 0, 1};
                if (tableAlpha) {
                    v = &bad; /* (not rendered: whatever reads it is left out) */
                } else {
                    capture_update();
                    v = sCapValid ? sCap : NULL;
                }
            } else {
                if (s == sRunTarget) { run_flush(); }
                v = sTgVer[s];
            }
            if (v == NULL) { continue; }
            if (v->alphaBad) {
                if (d->target == shown) { continue; }
                if (d->target != sRunTarget) {
                    run_flush();
                    sRunTarget = d->target;
                    sRunW = sRunH = 0;
                    sRunBad = 0;
                }
                sRunBad = 1;
                continue;
            }
            if (d->target == shown) { v->final = 1; }
            src = &v->t;
        }
        sDrawSrc[n] = src;
        if (d->target == shown) {
            sShownList[sShownCount++] = n;
            continue;
        }
        if (d->target != sRunTarget) {
            run_flush();
            sRunTarget = d->target;
            sRunW = sRunH = 0;
            sRunBad = 0;
        }
        sRun[sRunCount++] = n;
        {   /* the run's extent: the scissor, and for 2D the vertices inside it */
            uint32_t sx1 = (uint32_t)((d->scissor[0] + d->scissor[2]) / SCALE), sy1 = (uint32_t)((d->scissor[1] + d->scissor[3]) / SCALE);
            if (!d->vu && d->count > 0) {
                float mx = 0.0f, my = 0.0f;
                uint32_t j;
                for (j = 0; j < d->count; j++) {
                    const Vtx *v = &gsVerts[d->first + j];
                    if (v->x > mx) { mx = v->x; }
                    if (v->y > my) { my = v->y; }
                }
                if ((uint32_t)ceilf(mx) < sx1) { sx1 = (uint32_t)ceilf(mx); }
                if ((uint32_t)ceilf(my) < sy1) { sy1 = (uint32_t)ceilf(my); }
            }
            if (sx1 > sRunW) { sRunW = sx1; }
            if (sy1 > sRunH) { sRunH = sy1; }
        }
    }
    run_flush();
    render_run(sShownList, sShownCount, NULL, NULL);
    if (shown >= 0) { gsTargets[shown].cleared = 1; }
    for (i = 0; i < sOldCount; i++) { ver_free(sOldVer[i]); }
    sOldCount = 0;
    for (i = 0; i < MAX_TARGETS; i++) {
        if (sTgVer[i] != NULL) { sTgVer[i]->final = 0; }
    }
    if (sCap != NULL) { sCap->final = 0; }
    if (getenv("BT3_GS_VERBOSE") != NULL && gGsFrame % 30 == 0) {
        fprintf(stderr, "pvr: frame %u: %u renders, %u spills\n", gGsFrame, sRenders, sSpills);
    }
    sSpills = 0;
}

/* every draw of one frame (BT3_PVR_LOG=<frame>; BT3_PVR_LOGV=<draw> its vertices) */
static void draw_log(int shown) {
    uint32_t n;
    for (n = 0; n < gsDrawCount; n++) {
        const GsDraw *d = &gsDraws[n];
        const PvrTex *lt = d->mode[0] == 1 || d->mode[0] == 3 ? (const PvrTex *)d->tex : NULL;
        fprintf(stderr, "pvr: draw %u: native %d vu %d target %d%s first %u count %u key %08x mode %d %d %d %d tex %ux%u%s\n", n,
                d->native, d->vu, d->target, d->target == shown ? " (shown)" : "", d->first, d->count,
                d->pipeline >= 0 && d->pipeline < sPipeCount ? sPipeKey[d->pipeline] : 0, d->mode[0], d->mode[1], d->mode[2], d->mode[3],
                lt ? lt->w : 0, lt ? lt->h : 0, lt && !lt->mem ? " (none)" : "");
        if (d->mode[0] == 2) {
            fprintf(stderr, "pvr:   samples buffer %d\n", (int)((const PvrTex *)d->tex - sTgCol));
        }
        if (!d->vu && d->count > 0 && getenv("BT3_PVR_LOGV") != NULL && n == (uint32_t)atoi(getenv("BT3_PVR_LOGV"))) {
            uint32_t j;
            for (j = 0; j < d->count && j < 24; j++) {
                const Vtx *w = &gsVerts[d->first + j];
                fprintf(stderr, "pvr:     v%u %.1f %.1f z %.6f rgba %u %u %u %u stq %g %g %g u %g v %g\n", j, w->x, w->y, w->z, w->r, w->g, w->b, w->a, w->s, w->t, w->q, w->s / w->q, w->t / w->q);
            }
        }
        if (d->vu && d->count > 0 && getenv("BT3_PVR_LOGV") != NULL && n == (uint32_t)atoi(getenv("BT3_PVR_LOGV"))) {
            uint32_t j;
            for (j = 0; j < d->count && j < 12; j++) {
                const float *w = &gsVuVerts[gsVuIdx[d->first + j] * 12];
                fprintf(stderr, "pvr:     i%u=%u pos %.2f %.2f %.2f %.2f col %.1f %.1f %.1f %.1f st %.4f %.4f %.4f %.4f\n", j, gsVuIdx[d->first + j],
                        w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
            }
        }
        if (d->vu) {
            fprintf(stderr, "pvr:   blendc %.2f misc %.0f %.0f %.0f %.0f sampler %d\n", d->blendc, d->misc[0], d->misc[1], d->misc[2], d->misc[3], d->sampler);
        }
        if (!d->vu && d->count > 0) {
            const Vtx *v = &gsVerts[d->first];
            fprintf(stderr, "pvr:   first vertex %.1f %.1f z %.6f rgba %u %u %u %u st %.3f %.3f %.3f; blendc %.2f misc %.0f %.0f %.0f %.0f\n", v->x, v->y, v->z,
                    v->r, v->g, v->b, v->a, v->s, v->t, v->q, d->blendc, d->misc[0], d->misc[1], d->misc[2], d->misc[3]);
        }
    }
}

extern unsigned gRndOtStat[4]; /* rnd_gs.c */
static uint32_t sPalKept, sPalUsed, sPalUsedPeak; /* the palettes' colours: the blocks kept for them (pal_col_new), the
                                                   slots in use, and the most */

static void frame_end(void) {
    int i, shown;

    GsGpu_TitlePoll();
    GsDraw_Events();
    if (getenv("BT3_GS_VERBOSE") != NULL && gGsFrame % 30 == 0) {
        fprintf(stderr, "pvr: frame %u: %u draws, %u vertices, %d targets, %d textures + %u native (%u KB in PVR memory, at most %u KB; %u palette banks taken in use), %d pipelines; not native: %u draws with nothing at the block, %u not converted, %u of another size, %u uploads, %u palette uploads (records: %u not 4/8-bit, %u packet, %u not in the pack, %u size, %u full); palettes made: %u from tex.pak, %u read out of GS memory; %u draws tinted; %u native let go of PVR memory; %u records from GS memory, %u by TEX0 alone, %u whose handle named nothing (%u whose palette was no longer at its CBP; %u bindings, at most %u); ordering table: %u packets, %u decoded, %u records, %u nodes read as GS packets; palettes' colours: %u KB kept, %u KB in use (at most %u)\n",
                gGsFrame, gsDrawCount, gsVertCount + gsVuVertCount, gsTargetCount, gsTexCount, sNatLive, sTexBytes >> 10, sTexPeak >> 10, sNatSteal, sPipeCount,
                sNatMiss[0], sNatMiss[1], sNatMiss[2], sNatMiss[3], sPalMiss, sNatWhy[0], sNatWhy[1], sNatWhy[2], sNatWhy[3], sNatWhy[4], sPalMade[0], sPalMade[1], sTintDraws, sNatEvicted, gsRndFallback, gsRndByTex0, gsRndHandleMiss, gRndRebound, gRndBindLive, gRndBindPeak, gRndOtStat[0], gRndOtStat[1], gRndOtStat[2], gRndOtStat[3], sPalKept >> 10, sPalUsed >> 10, sPalUsedPeak >> 10);
        sTintDraws = 0;
        gRndOtStat[0] = gRndOtStat[1] = gRndOtStat[2] = gRndOtStat[3] = 0;
        gsRndFallback = gsRndByTex0 = gsRndHandleMiss = 0;
        memset(sNatMiss, 0, sizeof(sNatMiss));
        sPalMiss = 0;
        gRndRebound = 0;
        memset(sPalMade, 0, sizeof(sPalMade));
    }
    memset(gGsTint, 0, sizeof(gGsTint)); /* (the tints are the frame's: the game sets them again for the next) */

    shown = shown_target();

    if (getenv("BT3_PVR_LOG") != NULL && (int)gGsFrame == atoi(getenv("BT3_PVR_LOG"))) { draw_log(shown); }
    frame_render(shown);
    late_release();
    {   /* BT3_SHOT=<n>: every n-th frame of the game to port/build/shots/pvr_NNNNN.ppm (the other back ends' gpu_*),
           BT3_SHOT_FROM / BT3_SHOT_TO limit the frames */
        static int every = -1;
        if (every < 0) { every = getenv("BT3_SHOT") != NULL ? atoi(getenv("BT3_SHOT")) : 0; }
        if (every > 0 && gGsFrame % (unsigned)every == 0 &&
            (getenv("BT3_SHOT_FROM") == NULL || (int)gGsFrame >= atoi(getenv("BT3_SHOT_FROM"))) &&
            (getenv("BT3_SHOT_TO") == NULL || (int)gGsFrame <= atoi(getenv("BT3_SHOT_TO")))) {
#ifndef __DREAMCAST__ /* (gpu_pvr's picture; on the Dreamcast it is in video memory) */
            extern int kos_pvr_read_front(uint8_t *rgb, int stride, int w, int h);
            static uint8_t rgb[640 * 480 * 3];
            char name[64];
            FILE *fp;
            snprintf(name, sizeof(name), "port/build/shots/pvr_%05u.ppm", gGsFrame);
            if (kos_pvr_read_front(rgb, 640 * 3, 640, 480) && (fp = fopen(name, "wb")) != NULL) {
                fprintf(fp, "P6\n640 480\n255\n");
                fwrite(rgb, 1, sizeof(rgb), fp);
                fclose(fp);
            }
#endif
        }
    }

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


/* ---- the game's texture entries as PVR textures of their own ------------------------------------------------
   A texture entry (TexEntry, include/sys/gfxm_b_b.h) the game uploads becomes a PVR texture the first time, from
   the converted ones (texpak.h), and stays one until the game frees its file: the upload itself is left out
   (Port_TexNative), nothing is decoded or sent again. Its record is found by the entry's address (sNatHash; the
   entry itself is not written: the HUD's keeps a mark at +0x28); its handle is bound to the block the game
   uploads it to (port/src/gs_marker.c Port_Bind), for the records that name it by TEX0.
   The palette stays the game's (it changes them): read from GS memory when it changes, into the codebook of
   an 8-bit one (VQ, the palette as the codebook) or into a bank of palette RAM for a 4-bit one (PAL4). */
/* (the attract demo's 1 v 1 fight has about 170 at once; a full table says so, nat_full: raise it then) */
#define MAX_NAT 1024
typedef struct NatTex {
    PvrTex t;          /* as it is drawn */
    uint32_t e;        /* the entry's game address; 0 = a free record */
    uint32_t psm, tw, th, bits;
    uint32_t palKey;   /* the palette the codebook or bank holds (clut hash), with palOk */
    int palOk, bank;   /* bank: a 4-bit one's, -1 none */
    int none;          /* not among the converted ones: uploaded as before */
    uint16_t next;     /* the next record of its sNatHash chain + 1 */
    uint32_t sample;   /* pkt_sample of its pixels' packet when it was made */
    uint16_t var;      /* 8-bit: the next copy for another palette + 1 (a record of its own, not in sNatHash) */
    int isVar, pak;    /* a copy; the converted one's number in the pack */
    unsigned palFrame; /* 8-bit: the last frame drawn with the codebook */
    unsigned drawn;    /* the last frame drawn; a native one let go of its PVR memory (t.mem NULL) is read again */
    int direct;        /* a 32-, 24- or 16-bit one: 16-bit PVR texels, no palette (bits: the entry's) */
    int viewPak;       /* direct: its 8-bit view in the pack (the light), -1 none */
    uint16_t view;     /* direct: its first 8-bit view at a draw's size + 1 (a record of its own: isView) */
    int isView;        /* an 8-bit view of a direct one: its pak the view's, vw x vh of it at the corner */
    uint32_t vw, vh;
    uint16_t gen;      /* the record's use: a handle names it with this (Port_TexNative), and outlives it */
    uint8_t why;       /* none: why (NAT_*), for what Port_TexNative says */
} NatTex;
enum { NAT_MISSING, NAT_FORMAT, NAT_PACKET, NAT_SIZE, NAT_NO_PVR, NAT_UNREAD };
#define KEY_DIRECT 0x9E3779B97F4A7C15ull /* (as port/tools/dc_tex.c) */
#define KEY_T8     0xC2B2AE3D27D4EB4Full
/* A texture's key: its upload's hash and the shape TEX0 reads it as (TBW, PSM, TW, TH): one upload drawn as two
   shapes is two textures (the loading screen's 256 x 128 and 128 x 256 of the same data; as port/tools/dc_tex.c). */
#define KEY_SHAPE(key, t0) ((key) ^ ((uint64_t)(((t0) >> 14) & 0xFFFFF) + 1) * 0xD6E8FEB86659FD93ull)
static NatTex sNat[MAX_NAT];

/* No record free for a texture (MAX_NAT): it is drawn the old way, or not at all. */
static void nat_full(uint32_t addr) {
    Port_Lost("a texture record: the table is full (MAX_NAT, gs_pvr.c)", Port_Addr(addr), __builtin_return_address(0));
}
static uint16_t sNatGen;
static uint16_t sNatHash[MAX_NAT]; /* by the entry's address (its bits from 6: entries are 0x40 bytes): record + 1 */
#define NAT_HASH(addr) (((addr) >> 6) * 2654435761u >> 20 & (MAX_NAT - 1))
/* the palette RAM: 64 banks of 16 colours, by palette (clut hash), each free once no frame used it for two
   (the chip may still be drawing the frame before) */
static struct { uint32_t key; unsigned last; int used; } sBank[64];

static uint16_t argb4444(uint32_t c) { /* (alpha as the GS has it: 0x80 = opaque) */
    uint32_t a = (c >> 24) >= 0x80 ? 15 : (c >> 24) >> 3;
    return (uint16_t)(a << 12 | ((c >> 4) & 15) << 8 | ((c >> 12) & 15) << 4 | ((c >> 20) & 15));
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; } /* (any alignment; no memcpy call) */

/* What a record's packet holds, in brief: its size and 16 bytes from the middle and the end of it. The game can
   put another file where one was without freeing it (a copy within its heap): an entry or a packet at the same
   address is then another one. */
static uint32_t pkt_sample(const uint8_t *p, uint32_t size) {
    uint32_t h = 2166136261u ^ size, i;
    if (size < 32) { return h; }
    for (i = 0; i < 16; i++) { h = (h ^ p[size / 2 + i]) * 16777619u; h = (h ^ p[size - 16 + i]) * 16777619u; }
    return h;
}

/* The hash the converted ones are keyed by (port/tools/dc_tex.c): the upload packet's image data, its first
   width x height pixels. A palette of 32 bits (pal) leaves out the alpha of its first 16 colours: the game writes
   its objects' alpha table slots there (gfxm_e_b.c BtlObjMdl_InitAlphaSlots), and Port_PalNative puts the live
   ones in. *img: where the image data starts. 0 = not such a packet. */
static int pkt_key(const uint8_t *p, uint32_t size, uint32_t blt, uint64_t *key, int pal, const uint8_t **img) {
    uint32_t dpsm = (blt >> 8) & 63, bits = dpsm == 0 ? 32 : dpsm == 1 ? 24 : dpsm == 2 || dpsm == 0xA ? 16 : dpsm == 0x13 ? 8 : 4;
    uint32_t rw = 0, rh = 0, at = 0, want = 0, images = 0, i, skip = pal && dpsm == 0 ? 64 : 0;
    uint64_t h = 0xCBF29CE484222325ull;
    if (size >= 16 && (p[15] & 0x7F) == 0x50) { at = 16; } /* VIF DIRECT */
    while (at + 16 <= size) {
        uint64_t tag = (uint64_t)rd32(p + at) | (uint64_t)rd32(p + at + 4) << 32;
        uint32_t nloop = tag & 0x7FFF, flg = (tag >> 58) & 3, nreg = (tag >> 60) & 15, n;
        at += 16;
        if (flg == 0 && nreg == 1 && (p[at - 8] & 15) == 0xE) { /* A+D: TRXREG */
            for (i = 0; i < nloop && at + 16 <= size; i++, at += 16) {
                if (p[at + 8] == 0x52) { rw = rd32(p + at) & 0xFFF; rh = rd32(p + at + 4) & 0xFFF; }
            }
        } else if (flg == 2) { /* IMAGE */
            if (images++ == 0) { want = rw * rh * bits / 8; if (img != NULL) { *img = p + at; } }
            n = nloop * 16 < size - at ? nloop * 16 : size - at;
            for (i = 0; i < n && want != 0; i++, want--) {
                if (rw * rh * 4 - want < skip && ((rw * rh * 4 - want) & 3) == 3) { continue; } /* (a slot alpha) */
                h = (h ^ p[at + i]) * 0x100000001B3ull;
            }
            at += nloop * 16;
        } else {
            break;
        }
    }
    *key = h;
    return images != 0 && rw != 0;
}

void Port_TexFreed(uint32_t lo, uint32_t hi);
static int nat_fill(NatTex *n);

/* The record of entry e (made the first time: its PVR memory filled from the converted one). */
static NatTex *nat_get(uint8_t *e) {
    uint32_t addr = Port_Addr(e), i, w, h, bits, psm, tw, th, bytes;
    uint64_t t0, key;
    NatTex *n;
    int k;

    for (i = sNatHash[NAT_HASH(addr)]; i != 0; i = sNat[i - 1].next) {
        if (sNat[i - 1].e == addr) {
            if (sNat[i - 1].sample == pkt_sample(Port_Ptr(rd32(e + 0x38)), rd32(e + 8))) { return &sNat[i - 1]; }
            Port_TexFreed(addr, addr + 1); /* (another entry there now) */
            break;
        }
    }
    for (i = 0; i < MAX_NAT && sNat[i].e != 0; i++) {}
    if (i == MAX_NAT) { sNatWhy[4]++; nat_full(addr); return NULL; }
    n = &sNat[i];
    memset(n, 0, sizeof(*n));
    n->e = addr;
    n->gen = ++sNatGen != 0 ? sNatGen : ++sNatGen;
    n->bank = -1;
    n->next = sNatHash[NAT_HASH(addr)];
    sNatHash[NAT_HASH(addr)] = (uint16_t)(i + 1);
    n->sample = pkt_sample(Port_Ptr(rd32(e + 0x38)), rd32(e + 8));
    memcpy(&t0, e + 0x30, 8);
    psm = (t0 >> 20) & 63;
    tw = 1u << ((t0 >> 26) & 15);
    th = 1u << ((t0 >> 30) & 15);
    n->none = 1;
    n->viewPak = -1;
    if (psm == 0x00 || psm == 0x01 || psm == 0x02 || psm == 0x0A) { /* 16-bit PVR texels, and its 8-bit view */
        uint32_t fmt, vb;
        if (!pkt_key(Port_Ptr(rd32(e + 0x38)), rd32(e + 8), rd32(e + 0x18), &key, 0, NULL)) { sNatWhy[1]++; n->why = NAT_PACKET; return n; }
        key = KEY_SHAPE(key, t0);
        if ((k = TexPak_Find(key ^ KEY_DIRECT, &w, &h, &bits)) < 0) { goto missing; }
        if (w != (tw < 8 ? 8 : tw) || h != (th < 8 ? 8 : th) || bits != 16) { sNatWhy[3]++; n->why = NAT_SIZE; return n; }
        fmt = TexPak_Psm(k) >> 6;
        n->direct = 1;
        n->pak = k;
        n->psm = psm; n->tw = tw; n->th = th; n->bits = psm == 0x00 ? 32 : psm == 0x01 ? 24 : 16;
        n->t.w = w; n->t.h = h; n->t.format = GS_TEXFMT_RGBA8; n->t.bytes = w * h * 2;
        n->t.pw = w; n->t.ph = h;
        n->t.fmt = (fmt == 0 ? PVR_TXRFMT_ARGB1555 : fmt == 1 ? PVR_TXRFMT_RGB565 : PVR_TXRFMT_ARGB4444) | PVR_TXRFMT_TWIDDLED;
        if ((n->viewPak = TexPak_Find(key ^ KEY_T8, &n->vw, &n->vh, &vb)) >= 0 && vb != 8) { n->viewPak = -1; }
        if ((n->t.mem = pvr_alloc(n->t.bytes, "the direct entry", addr)) == NULL) { n->why = NAT_NO_PVR; return n; }
        if (!nat_fill(n)) { pvr_mem_free(n->t.mem); n->t.mem = NULL; n->why = NAT_UNREAD; return n; }
        n->t.base = n->t.mem;
        n->none = 0;
        sTexBytes += n->t.bytes;
        sNatLive++;
        if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
        return n;
    }
    if (psm != 0x13 && psm != 0x14) { sNatWhy[0]++; n->why = NAT_FORMAT; return n; }
    if (!pkt_key(Port_Ptr(rd32(e + 0x38)), rd32(e + 8), rd32(e + 0x18), &key, 0, NULL)) { sNatWhy[1]++; n->why = NAT_PACKET; return n; }
    key = KEY_SHAPE(key, t0);
    if ((k = TexPak_Find(key, &w, &h, &bits)) < 0) {
    missing:
        sNatWhy[2]++;
        n->why = NAT_MISSING;
        if (getenv("BT3_TEX_LOG") != NULL) { /* (to find where it came from: its packet's bytes from 0x60) */
            const uint8_t *q = Port_Ptr(rd32(e + 0x38));
            uint32_t j, sz = rd32(e + 8);
            fprintf(stderr, "nat: frame %u: entry %08X psm %X %ux%u packet %u bytes not in the pack:", (unsigned)gGsFrame, (unsigned)addr, (unsigned)psm, (unsigned)tw, (unsigned)th, (unsigned)sz);
            for (j = sz / 2; j < sz / 2 + 24 && j < sz; j++) { fprintf(stderr, " %02X", q[j]); }
            fprintf(stderr, "\n");
        }
        return n;
    }
    if (w != (tw < 8 ? 8 : tw) || h != (th < 8 ? 8 : th) || bits != (psm == 0x14 ? 4u : 8u)) { sNatWhy[3]++; n->why = NAT_SIZE; return n; }
    bytes = bits == 4 ? w * h / 2 : 2048 + w * h; /* (8-bit: the codebook, then the indices) */
    n->pak = k;
    n->psm = psm; n->tw = tw; n->th = th; n->bits = bits;
    n->t.w = w; n->t.h = h; n->t.format = GS_TEXFMT_RGBA8; n->t.bytes = bytes;
    if ((n->t.mem = pvr_alloc(bytes, bits == 4 ? "the 4-bit entry" : "the 8-bit entry", addr)) == NULL) { n->why = NAT_NO_PVR; return n; }
    if (!nat_fill(n)) { pvr_mem_free(n->t.mem); n->t.mem = NULL; n->why = NAT_UNREAD; return n; }
    n->none = 0;
    n->t.base = n->t.mem;
    n->t.pw = bits == 4 ? w : w * 2;
    n->t.ph = bits == 4 ? h : h * 2;
    n->t.fmt = bits == 4 ? PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_TWIDDLED : PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
    sTexBytes += bytes;
    sNatLive++;
    if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    return n;
}

/* The game uploads entry e's pixels to block tbp (gfxm_b_b.c, gfxm_c.c, gfxm_d_c.c): not 0 (its handle, include/port/rnd.h) = it is a PVR texture of
   its own, the upload is left out. Its handle is bound to the block (Port_Bind), for the records that name it by
   TEX0; in its place in the list a write of register 0x7E, which says which blocks the upload would have written
   (gs_walk.c: their pages changed). */
int Port_PalNative(void *pkt, int size, uint32_t blt, int block);
int Port_TexNative(void *e, int tbp) {
    extern void Port_MarkReg(uint32_t reg, uint64_t v); /* gs_marker.c */
    uint32_t blocks;
    NatTex *n;
    if (!nat_on()) { return 0; }
    if ((n = nat_get(e)) == NULL || n->none) {
        static const char *const kWhy[] = {
            "a texture not in tex.pak (its entry's address)", "a texture of a format tex.pak does not have (its entry's address)",
            "a texture whose upload packet was not read (its entry's address)", "a texture of another size in tex.pak (its entry's address)",
            "a texture: no PVR memory for it (its entry's address)", "a texture: tex.pak could not be read (its entry's address)"};
        sNatMiss[3]++;
        Port_Lost(n == NULL ? "a texture: no record for it (MAX_NAT; its entry's address)" : kWhy[n->why], Port_Addr(e), __builtin_return_address(0));
        return 0;
    }
    blocks = (uint32_t)(n - sNat + 1) | ((n->tw * n->th * n->bits / 8 + 255) / 256) << 16; /* (the blocks it covers) */
    sPalAsk = 1;
    if (n->direct && Port_PalNative(Port_Ptr(rd32((uint8_t *)e + 0x38)), (int)rd32((uint8_t *)e + 8), rd32((uint8_t *)e + 0x18), tbp)) {
        blocks &= 0xFFFF; /* (also a palette of the pack, which its marker just said: this one clears nothing) */
    }
    sPalAsk = 0;
    Port_MarkReg(0x7E, ((uint32_t)tbp & 0x3FFF) | (uint64_t)(blocks >> 16) << 48);
    Port_Bind(0, (uint32_t)tbp, blocks >> 16, (uint32_t)(n - sNat + 1) | (uint32_t)n->gen << 16);
    return (int)((uint32_t)(n - sNat + 1) | (uint32_t)n->gen << 16);
}

/* The palettes of tex.pak, by the address of the upload packet the game sends. */
#define MAX_PAL 2048
typedef struct NatPal {
    uint32_t addr;   /* the packet's game address; 0 = a free record */
    int pak;         /* its number in the pack; -1 = not a palette of it */
    uint32_t n, gen; /* colours (16 / 256); gen: the record's use, for GsPvr_PalKey */
    uint32_t *col;   /* as GS memory holds them, in the order the indices count */
    uint32_t sample; /* pal_sample of the packet when it was made */
    uint16_t next;   /* the next record of its sPalHash chain + 1 */
    uint16_t sub[4]; /* 256 colours of 32 bits uploaded as 16 x 16: the four palettes of 16 its four blocks hold (records
                        + 1, at addr + 1 + k: the font's, src/battle/col_c.c Font_MakeTex0) */
    uint16_t pktSize; /* pack-backed packets of this size can be reused without re-reading; writes invalidate them */
} NatPal;
static NatPal sPal[MAX_PAL];

/* The palettes' colours: out of blocks of PAL_BLOCK bytes kept for them, in slots of 1 KB (256 colours) and of 64
   bytes (16), a free list for each; not a block of their own each (hundreds of small ones that chop the memory up).
   sPalKept: the blocks taken (BT3_GS_VERBOSE says it). */
#define PAL_BLOCK 16384
static void *sPalFree[2];

static uint32_t *pal_col_new(uint32_t n) {
    int big = n == 256;
    uint32_t size = big ? 1024 : 64, i;
    void *p;
    if (sPalFree[big] == NULL) {
        uint8_t *b = malloc(PAL_BLOCK);
        if (b == NULL) {
            Port_Lost("a palette: no memory for another block of palettes' colours (PAL_BLOCK)", PAL_BLOCK, __builtin_return_address(0));
            return NULL;
        }
        sPalKept += PAL_BLOCK;
        for (i = 0; i < PAL_BLOCK; i += size) {
            *(void **)(b + i) = sPalFree[big];
            sPalFree[big] = b + i;
        }
    }
    p = sPalFree[big];
    sPalFree[big] = *(void **)p;
    sPalUsed += size;
    if (sPalUsed > sPalUsedPeak) { sPalUsedPeak = sPalUsed; }
    return p;
}

static void pal_col_free(uint32_t *col, uint32_t n) {
    if (col != NULL) {
        sPalUsed -= n == 256 ? 1024 : 64;
        *(void **)col = sPalFree[n == 256];
        sPalFree[n == 256] = col;
    }
}
static uint16_t sPalHash[MAX_PAL];
static uint32_t sPalGen;
#define PAL_HASH(addr) (((addr) >> 4) * 2654435761u >> 21 & (MAX_PAL - 1))

/* A free record, its address addr, in the hash; NULL = none. */
static NatPal *pal_new(uint32_t addr) {
    uint32_t i;
    NatPal *p;
    for (i = 0; i < MAX_PAL && sPal[i].addr != 0; i++) {}
    if (i == MAX_PAL) { return NULL; }
    p = &sPal[i];
    memset(p, 0, sizeof(*p));
    p->addr = addr;
    p->gen = ++sPalGen;
    p->pak = -1;
    p->next = sPalHash[PAL_HASH(addr)];
    sPalHash[PAL_HASH(addr)] = (uint16_t)(i + 1);
    return p;
}

/* p's 256 colours as the GS holds a 16 x 16 upload of 32 bits: four blocks of 8 x 8, each a palette of 16 for a 4-bit
   texture in its top 8 x 2 (blocks 0 1 / 2 3). Each a record of its own (p->sub). */
static void pal_split(NatPal *p) {
    uint32_t k, j;
    for (k = 0; k < 4; k++) {
        NatPal *q = pal_new(p->addr + 1 + k);
        if (q == NULL || (q->col = pal_col_new(16)) == NULL) { if (q != NULL) { Port_TexFreed(q->addr, q->addr + 1); } return; }
        q->pak = p->pak;
        q->n = 16;
        for (j = 0; j < 16; j++) {
            uint32_t x = (k & 1) * 8 + (j & 7), y = (k >> 1) * 8 + (j >> 3), at = y * 16 + x;
            q->col[j] = p->col[(at & 0xE7) | (at & 8) << 1 | (at & 0x10) >> 1]; /* (p's order: bits 3 and 4 exchanged) */
        }
        p->sub[k] = (uint16_t)(q - sPal + 1);
    }
}

/* A palette packet's record sample: pkt_sample's and its first 256 bytes (the tags and the first colours: the game
   writes the alpha of its first 16 while it runs, BtlObjMdl_InitAlphaSlots, which pkt_sample may not see). */
static uint32_t pal_sample(const uint8_t *p, uint32_t size) {
    uint32_t h = pkt_sample(p, size), i;
    for (i = 0; i < size && i < 256; i++) { h = (h ^ p[i]) * 16777619u; }
    return h;
}

/* The alpha of the first 16 colours of a 32-bit palette upload (img: its image data) into p's colours: the pack's
   are keyed without them (pkt_key), these are the ones the GS gets. */
static void pal_alphas(NatPal *p, const uint8_t *img) {
    uint32_t k, i;
    for (k = 0; k < 16; k++) {
        i = p->n == 16 ? k : (k & 0xE7) | (k & 8) << 1 | (k & 0x10) >> 1; /* (256: bits 3 and 4 exchanged) */
        p->col[i] = (p->col[i] & 0xFFFFFFu) | (uint32_t)img[k * 4 + 3] << 24;
    }
}

/* The game has just queued the upload of pkt to block `block` (gfxm_b_b.c, gfxm_c.c, gfxm_d_c.c, hud_c_c.c,
   eft_det_a.c): not 0 (its handle) = it is a palette of the pack, not to be sent; bound to the block, and a write
   of register 0x7D in its place, as Port_TexNative. */
int Port_PalNative(void *pkt, int size, uint32_t blt, int block) {
    extern void Port_MarkReg(uint32_t reg, uint64_t v); /* gs_marker.c */
    uint32_t addr = Port_Addr(pkt), i, w, h, bits, k4, hnd;
    uint64_t key;
    const uint8_t *img = NULL;
    NatPal *p = NULL;
    int k;

    if (!nat_on()) { return 0; }
    for (i = sPalHash[PAL_HASH(addr)]; i != 0; i = sPal[i - 1].next) {
        if (sPal[i - 1].addr == addr) {
            NatPal *old = &sPal[i - 1];
            /* Pack-backed model palettes are immutable between invalidations. Their only in-place edit is the
               16 alpha slots below; Heap_Free/file overwrite and BtlObjMdl_InitAlphaSlots call Port_TexFreed. */
            if (old->pak >= 0 && size >= 0 && size < 65536 && old->pktSize == (uint16_t)size) { p = old; break; }
            if (old->sample == pal_sample(pkt, (uint32_t)size)) { p = old; break; }
            Port_TexFreed(addr, addr + 16); /* (another packet there now; and the four palettes of its blocks) */
            break;
        }
    }
    if (p == NULL) {
        if ((p = pal_new(addr)) == NULL) {
            sPalMiss += !sPalAsk;
            if (!sPalAsk) { Port_Lost("a palette: no room for another (MAX_PAL)", addr, __builtin_return_address(0)); }
            return 0;
        }
        p->sample = pal_sample(pkt, (uint32_t)size);
        if (pkt_key(pkt, (uint32_t)size, blt, &key, 1, &img) && (k = TexPak_Find(key, &w, &h, &bits)) >= 0 && bits == 32 && h == 1 &&
            (w == 16 || w == 256) && TexPak_Psm(k) == ((blt >> 8) & 63)) {
            p->pktSize = size < 65536 ? (uint16_t)size : 0;
            if ((p->col = pal_col_new(w)) == NULL || !TexPak_ReadInto(k, p->col, w * 4)) {
                /* (no memory for it, said by pal_col_new, or the file failed: not kept, asked again next time) */
                if (p->col != NULL && !sPalAsk) {
                    Port_Lost("a palette: tex.pak could not be read (its packet's address)", addr, __builtin_return_address(0));
                }
                pal_col_free(p->col, w);
                p->col = NULL;
                Port_TexFreed(addr, addr + 1);
                sPalMiss += !sPalAsk;
                return 0;
            }
            p->pak = k;
            p->n = w;
            if (TexPak_Psm(k) == 0 && img + 64 <= (const uint8_t *)pkt + size) { pal_alphas(p, img); }
            if (w == 256 && TexPak_Psm(k) == 0) { pal_split(p); }
        }
    }
    if (p->pak < 0) {
        sPalMiss += !sPalAsk;
        if (!sPalAsk) { Port_Lost("a palette not in tex.pak (its packet's address)", addr, __builtin_return_address(0)); }
        if (!sPalAsk && getenv("BT3_TEX_LOG") != NULL) {
            fprintf(stderr, "nat: frame %u: palette packet %08X (%d bytes) to block %X not in the pack\n", (unsigned)gGsFrame, (unsigned)addr, size, (unsigned)block);
        }
        return 0;
    }
    hnd = (uint32_t)(p - sPal + 1) | (p->gen & 0xFFFF) << 16;
    Port_MarkReg(0x7D, ((uint32_t)block & 0x3FFF) | (uint64_t)(p->n == 256 ? 4u : 1u) << 48); /* (the blocks it covers) */
    Port_Bind(1, (uint32_t)block, p->n == 256 ? 4u : 1u, hnd);
    if (p->sub[0] != 0 && (block & 3) == 0) {
        /* (and blocks 1 to 3 their palettes of 16, for a 4-bit texture that names them; block 0's is p->sub[0]) */
        for (k4 = 1; k4 < 4; k4++) {
            Port_Bind(1, (uint32_t)(block + (int)k4), 0, Port_PalSub(hnd, (int)k4));
        }
    }
    return (int)hnd;
}

/* The colours of the palette handle pal names, if it has n: in the order the indices count, as GS memory would
   hold them. NULL = none (gs_draw.c's table passes). */
const uint32_t *GsPvr_PalCol(uint32_t pal, uint32_t n) {
    uint32_t pi = pal & 0xFFFF;
    if (pi == 0 || pi > MAX_PAL || sPal[pi - 1].addr == 0 || (sPal[pi - 1].gen & 0xFFFF) != pal >> 16 || sPal[pi - 1].n != n) {
        return NULL;
    }
    return sPal[pi - 1].col;
}

/* src/sys/gfxm_d_c.c ObjDraw_AddClutPass: the palettes of the two pages from block cbp are tinted this frame (k, o:
   per channel, colour * k + o in 0..255; alpha >= 0: their alpha replaced by it, 0x80 = opaque). 0 = not done so
   (the game draws its passes over them in GS memory as before). cbp < 0: no pass this frame (1: none drawn). */
int Port_ClutTint(int cbp, const float *k, const float *o, int alpha) {
    extern void Port_MarkReg(uint32_t reg, uint64_t v); /* gs_marker.c */
    uint64_t v = 1ull << 63;
    int i;
    if (!nat_on()) { return 0; }
    if (cbp < 0) { return 1; } /* (no pass this frame: nothing to tint) */
    for (i = 0; i < 3; i++) {
        float kk = k[i] < 0.0f ? 0.0f : k[i] > 1.0f ? 1.0f : k[i], oo = o[i] < 0.0f ? 0.0f : o[i] > 255.0f ? 255.0f : o[i];
        v |= (uint64_t)(uint32_t)(kk * 255.0f + 0.5f) << (i * 8) | (uint64_t)(uint32_t)(oo + 0.5f) << (24 + i * 8);
    }
    if (alpha >= 0) { v |= (uint64_t)gs_unit((uint32_t)alpha & 0xFF) << 48 | 1ull << 56; }
    Port_MarkReg(0x7B, v);
    Port_MarkReg(0x7C, ((uint32_t)cbp >> 5) & 511);
    return 1;
}

/* The game's heap block [lo, hi) is freed: the entries in it are no longer textures. */
void Vu1Native_Freed(uint32_t lo, uint32_t hi);
void Port_TexFreed(uint32_t lo, uint32_t hi) {
    uint32_t i;
    uint16_t *p;
    Vu1Native_Freed(lo, hi); /* (the models' chains kept: vu1_native.c) */
    for (i = 0; i < MAX_NAT; i++) {
        NatTex *n = &sNat[i];
        if (n->e == 0 || n->e < lo || n->e >= hi) { continue; }
        if (n->t.mem != NULL) { late_free(n->t.mem); sTexBytes -= n->t.bytes; sNatLive--; }
        if (!n->isVar && !n->isView) {
            for (p = &sNatHash[NAT_HASH(n->e)]; *p != i + 1; p = &sNat[*p - 1].next) {}
            *p = n->next;
        }
        n->e = 0;
    }
    for (i = 0; i < MAX_PAL; i++) { /* and the palettes' packets in it */
        NatPal *q = &sPal[i];
        if (q->addr == 0 || q->addr < lo || q->addr >= hi) { continue; }
        for (p = &sPalHash[PAL_HASH(q->addr)]; *p != i + 1; p = &sPal[*p - 1].next) {}
        *p = q->next;
        pal_col_free(q->col, q->n);
        q->col = NULL;
        q->addr = 0;
    }
}

/* PVR memory full: the native texture drawn longest ago (none drawn in the last two frames: the chip may still be
   drawing the one before) lets go of its PVR memory; it is read from the pack again when drawn. 0 = none to. */
static int nat_evict(void) {
    NatTex *best = NULL;
    uint32_t i;
    for (i = 0; i < MAX_NAT; i++) {
        NatTex *n = &sNat[i];
        if (n->e == 0 || n->none || n->t.mem == NULL || n->drawn + 2 > gGsFrame) { continue; }
        if (best == NULL || n->drawn < best->drawn) { best = n; }
    }
    if (best == NULL) { return 0; }
    pvr_mem_free(best->t.mem);
    best->t.mem = best->t.base = NULL;
    best->palOk = 0;
    sTexBytes -= best->t.bytes;
    sNatLive--;
    sNatEvicted++;
    return 1;
}

/* the pack's pieces straight into PVR memory (ctx: where the data goes) */
static void put_pvr(void *ctx, const uint8_t *piece, uint32_t at, uint32_t n) {
    pvr_txr_load(piece, (uint8_t *)ctx + at, n);
}

/* an 8-bit view's rows (vw x vh, in pieces of whole pairs of rows) into the corner of its twiddled w x h: in a
   twiddled 8-bit texture rows 2k and 2k + 1 of a column are side by side, one 16-bit write */
typedef struct { uint8_t *mem; uint32_t w, h, vw; } ViewPut;
static void put_view(void *ctx, const uint8_t *piece, uint32_t at, uint32_t n) {
    ViewPut *v = ctx;
    uint32_t y = at / v->vw, rows = n / v->vw, r, x;
    for (r = 0; r < rows && y < v->h; r += 2, y += 2) {
        const uint8_t *a = piece + r * v->vw, *b = r + 1 < rows ? a + v->vw : NULL;
        for (x = 0; x < v->vw && x < v->w; x++) {
            *(volatile uint16_t *)(v->mem + twiddle_index(x, y, v->w, v->h)) = (uint16_t)(a[x] | (b != NULL ? b[x] : 0) << 8);
        }
    }
}

/* n's texels from the pack into its PVR memory, a piece at a time (8-bit: after the codebook; a view: its vw x vh
   at the corner of the draw's size, twiddled, the rest 0). 0 = not read (said). */
static int nat_fill(NatTex *n) {
    uint32_t off = n->direct || n->bits == 4 ? 0 : 2048, i;
    uint8_t *mem = (uint8_t *)n->t.mem + off;
    int ok;
    if (n->isView) {
        ViewPut v;
        v.mem = mem; v.w = n->t.w; v.h = n->t.h; v.vw = n->vw;
        for (i = 0; i < n->t.bytes - off; i += 4) { *(volatile uint32_t *)(mem + i) = 0; }
        ok = TexPak_Bytes(n->pak) == n->vw * n->vh && TexPak_Stream(n->pak, 2 * n->vw, put_view, &v);
    } else {
        ok = TexPak_Bytes(n->pak) == n->t.bytes - off && TexPak_Stream(n->pak, 32, put_pvr, mem);
    }
    if (!ok) { Port_Lost("a texture: tex.pak could not be read, or not at the size the record has (its number in the pack)", (uint32_t)n->pak, __builtin_return_address(0)); }
    return ok;
}

/* A native one that let go of its PVR memory, read again. 0 = no room. */
static int nat_reload(NatTex *n) {
    if ((n->t.mem = pvr_alloc(n->t.bytes, "an entry read again", n->e)) == NULL) { return 0; }
    if (!nat_fill(n)) { pvr_mem_free(n->t.mem); n->t.mem = NULL; return 0; }
    n->t.base = n->t.mem;
    n->palOk = 0;
    sTexBytes += n->t.bytes;
    sNatLive++;
    if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    return 1;
}

/* Another copy of 8-bit n, for another palette (a frame can draw it with two, and the chip may still be drawing
   the frame before with the codebook). NULL = no room. */
static NatTex *nat_copy(NatTex *n) {
    uint32_t i;
    NatTex *v;
    for (i = 0; i < MAX_NAT && sNat[i].e != 0; i++) {}
    if (i == MAX_NAT) { nat_full(n->e); return NULL; }
    v = &sNat[i];
    *v = *n;
    v->palOk = 0;
    v->isVar = 1; /* (after n in its chain: v->var is n's next) */
    v->t.mem = v->t.base = pvr_alloc(n->t.bytes, "a copy for another palette of the entry", n->e);
    if (v->t.mem == NULL || !nat_fill(v)) {
        if (v->t.mem != NULL) { pvr_mem_free(v->t.mem); }
        v->e = 0;
        return NULL;
    }
    n->var = (uint16_t)(i + 1);
    sTexBytes += v->t.bytes;
    sNatLive++;
    if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    return v;
}

/* Direct one n read as an 8-bit texture of tw x th (the light): its view at that size, made the first time. */
static NatTex *nat_view(NatTex *n, uint32_t tw, uint32_t th) {
    uint32_t i;
    NatTex *v;
    for (i = n->view; i != 0; i = sNat[i - 1].var) {
        if (sNat[i - 1].tw == tw && sNat[i - 1].th == th) { return &sNat[i - 1]; }
    }
    for (i = 0; i < MAX_NAT && sNat[i].e != 0; i++) {}
    if (i == MAX_NAT) { nat_full(n->e); return NULL; }
    v = &sNat[i];
    memset(v, 0, sizeof(*v));
    v->e = n->e;
    v->bank = -1;
    v->viewPak = -1;
    v->isView = 1;
    v->pak = n->viewPak;
    v->vw = n->vw; v->vh = n->vh;
    v->psm = 0x13; v->tw = tw; v->th = th; v->bits = 8;
    v->t.w = tw < 8 ? 8 : tw; v->t.h = th < 8 ? 8 : th; v->t.format = GS_TEXFMT_RGBA8;
    v->t.bytes = 2048 + v->t.w * v->t.h;
    v->t.pw = v->t.w * 2; v->t.ph = v->t.h * 2;
    v->t.fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
    v->t.mem = v->t.base = pvr_alloc(v->t.bytes, "an 8-bit view of the entry", n->e);
    if (v->t.mem == NULL || !nat_fill(v)) {
        if (v->t.mem != NULL) { pvr_mem_free(v->t.mem); }
        v->e = 0;
        return NULL;
    }
    v->var = 0;
    v->drawn = gGsFrame;
    sTexBytes += v->t.bytes;
    sNatLive++;
    if (sTexBytes > sTexPeak) { sTexPeak = sTexBytes; }
    {   /* (its own chain of views by size: through var, which a view's copies for other palettes then also use) */
        v->var = n->view;
        n->view = (uint16_t)(i + 1);
    }
    return v;
}

/* Native record n drawn as TEX0 reads it (tbp: its block, which says whether a direct one can be read as 8-bit
   indices), with palette pal (palKey: what tells its colours apart). 0 = none. */
static GsTex nat_resolve(NatTex *n, uint32_t tbp, uint32_t psm, uint32_t tw, uint32_t th, const NatPal *pal, uint32_t cbp, uint32_t cpsm, uint32_t palKey);

/* include/port/rnd.h: the handle of entry e's texture record (one Port_TexNative made), of the palette record of
   upload packet pkt (Port_PalNative); 0 = none. */
uint32_t Port_TexHandle(const void *e) {
    uint32_t addr = Port_Addr((void *)e), i;
    for (i = sNatHash[NAT_HASH(addr)]; i != 0; i = sNat[i - 1].next) {
        if (sNat[i - 1].e == addr) { return sNat[i - 1].none ? 0 : (i | (uint32_t)sNat[i - 1].gen << 16); }
    }
    return 0;
}

uint32_t Port_PalHandle(const void *pkt) {
    uint32_t addr = Port_Addr((void *)pkt), i;
    for (i = sPalHash[PAL_HASH(addr)]; i != 0; i = sPal[i - 1].next) {
        if (sPal[i - 1].addr == addr) { return sPal[i - 1].pak < 0 ? 0 : (i | (sPal[i - 1].gen & 0xFFFF) << 16); }
    }
    return 0;
}

/* The handle of palette k (0..3) of 16 inside the 256 that handle pal names (its blocks as a 4-bit texture's
   palettes: the font's); 0 = none. */
uint32_t Port_PalSub(uint32_t pal, int k) {
    uint32_t pi = pal & 0xFFFF, i;
    if (pi == 0 || pi > MAX_PAL || sPal[pi - 1].addr == 0 || (sPal[pi - 1].gen & 0xFFFF) != pal >> 16 || k < 0 || k > 3 ||
        (i = sPal[pi - 1].sub[k]) == 0 || sPal[i - 1].addr == 0) {
        return 0;
    }
    return i | (sPal[i - 1].gen & 0xFFFF) << 16;
}

/* What a record the game handed over names (include/port/rnd.h): texture `tex` and palette `pal` as Port_TexNative
   and Port_PalNative gave them (or port/src/gs_marker.c bind_resolve, from what was bound where its TEX0 names),
   and its size. With the record's TEX0 (t0 not 0) it is read as that TEX0 reads it: its format and size (a direct
   one as 8-bit indices: the light), whose palette is then the one pal names. 0 = not (or no longer) one, or not
   drawable so: its GS fallback is drawn. */

/* ---- the movie (port/src/plat_movie.c): its pictures in a YUV422 texture of their own, RND_TEX_MOVIE ----
   Not twiddled, the picture's size rounded up to powers of two (the picture at its top left). The PC writes its
   texels from the decoder's planes; the Dreamcast hands the decoder's macroblocks to the PVR's YUV converter, which
   writes them. */
static PvrTex sMovie;
static uint32_t sMovieW, sMovieH; /* the picture */
void GsPvr_MovieEnd(void);

static uint32_t pow2_movie(uint32_t n) {
    uint32_t p = 8;
    while (p < n) { p <<= 1; }
    return p;
}

/* For pictures of w x h (multiples of 16, at most 512 x 512). 0 = no PVR memory for it. */
int GsPvr_MovieBegin(int w, int h) {
    uint32_t tw = pow2_movie((uint32_t)w), th = pow2_movie((uint32_t)h);
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || (w | h) & 15) { return 0; }
    if (sMovie.mem != NULL && (sMovie.w != tw || sMovie.h != th)) { GsPvr_MovieEnd(); }
    if (sMovie.mem == NULL) {
        sMovie.mem = pvr_alloc(tw * th * 2, "the movie", 0);
        if (sMovie.mem == NULL) { return 0; }
    }
    sMovieW = (uint32_t)w;
    sMovieH = (uint32_t)h;
    sMovie.w = sMovie.pw = tw;
    sMovie.h = sMovie.ph = th;
    sMovie.format = GS_TEXFMT_RGBA8;
    sMovie.fmt = PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED;
    sMovie.base = sMovie.mem;
    sMovie.bytes = tw * th * 2;
#ifdef __DREAMCAST__
    PVR_SET(PVR_YUV_ADDR, (uint32_t)sMovie.mem & 0xFFFFFF);
    PVR_SET(PVR_YUV_CFG, ((sMovieH / 16 - 1) << 8) | (tw / 16 - 1));
    PVR_GET(PVR_YUV_CFG);
    sq_set(sMovie.mem, 0, tw * th * 2); /* (black) */
#else
    {
        static uint32_t black[256];
        uint32_t y;
        for (y = 0; y < tw / 2; y++) { black[y] = 0x10801080u; } /* (Y 16, U V 128) */
        for (y = 0; y < th; y++) { pvr_txr_load(black, (uint8_t *)sMovie.mem + y * tw * 2, tw * 2); }
    }
#endif
    return 1;
}

void GsPvr_MovieEnd(void) {
    if (sMovie.mem != NULL) {
        pvr_mem_free(sMovie.mem);
        sMovie.mem = sMovie.base = NULL;
    }
}

#ifdef __DREAMCAST__
/* A picture as the decoder keeps it for the converter: its macroblocks of 384 bytes, rows first, each row padded
   to the texture's width (the converter is set for the picture's rows and the texture's width). */
void GsPvr_MovieMacroblocks(const uint32_t *mb) {
    int bw = (int)sMovieW >> 4, bh = (int)sMovieH >> 4, x, y, i;
    uint32_t *d;

    if (sMovie.mem == NULL) { return; }
    PVR_SET(PVR_YUV_CFG, ((sMovieH / 16 - 1) << 8) | (sMovie.w / 16 - 1)); /* (again each picture, as Quake2-main
                                                                              does for Flycast) */
    d = SQ_MASK_DEST((void *)PVR_TA_YUV_CONV);
    sq_lock((void *)PVR_TA_YUV_CONV);
    for (y = 0; y < bh; y++) {
        for (x = 0; x < bw; x++, mb += 96) { sq_fast_cpy(d, mb, 384 / 32); }
        for (i = 0; i < ((int)sMovie.w / 16 - bw) * (384 / 32); i++) { sq_flush(d); }
    }
    sq_unlock();
}
#else
/* A picture from the decoder's planes (Y, and Cb and Cr half as wide and high), their rows ys / cs bytes apart:
   each two pixels one word of the PVR's YUV422, U Y0 V Y1. */
void GsPvr_MovieYuv(const uint8_t *py, const uint8_t *pcb, const uint8_t *pcr, int ys, int cs) {
    static uint32_t row[256];
    uint32_t x, y;

    if (sMovie.mem == NULL) { return; }
    for (y = 0; y < sMovieH; y++) {
        const uint8_t *l = py + y * (uint32_t)ys, *u = pcb + (y >> 1) * (uint32_t)cs, *v = pcr + (y >> 1) * (uint32_t)cs;
        for (x = 0; x < sMovieW; x += 2) {
            row[x >> 1] = (uint32_t)u[x >> 1] | (uint32_t)l[x] << 8 | (uint32_t)v[x >> 1] << 16 | (uint32_t)l[x + 1] << 24;
        }
        pvr_txr_load(row, (uint8_t *)sMovie.mem + y * sMovie.w * 2, sMovieW * 2);
    }
}
#endif

/* The picture's size (0 x 0: no movie). */
void GsPvr_MovieSize(int *w, int *h) {
    *w = sMovie.mem != NULL ? (int)sMovieW : 0;
    *h = sMovie.mem != NULL ? (int)sMovieH : 0;
}

static GsTex tex_handle(uint32_t tex, uint32_t pal, uint64_t t0, uint32_t *tw, uint32_t *th) {
    uint32_t ti = tex & 0xFFFF, pi = pal & 0xFFFF, psm, w, h;
    NatTex *n;
    const NatPal *p = NULL;

    if (tex == RND_TEX_MOVIE) { /* (the movie's picture) */
        if (sMovie.mem == NULL) { return 0; }
        *tw = sMovie.w;
        *th = sMovie.h;
        return (GsTex)&sMovie;
    }

    if (ti == 0 || ti > MAX_NAT || sNat[ti - 1].e == 0 || sNat[ti - 1].none || sNat[ti - 1].gen != tex >> 16) { return 0; }
    n = &sNat[ti - 1];
    psm = t0 != 0 ? (uint32_t)(t0 >> 20) & 0x3F : n->psm;
    w = t0 != 0 ? 1u << ((t0 >> 26) & 15) : n->tw;
    h = t0 != 0 ? 1u << ((t0 >> 30) & 15) : n->th;
    if (!n->direct || psm != n->psm) { /* (indices: their palette) */
        if (pi == 0 || pi > MAX_PAL || sPal[pi - 1].addr == 0 || (sPal[pi - 1].gen & 0xFFFF) != pal >> 16 ||
            sPal[pi - 1].n != (psm == 0x14 ? 16u : 256u)) { return 0; }
        p = &sPal[pi - 1];
    }
    *tw = w;
    *th = h;
    return nat_resolve(n, t0 != 0 ? (uint32_t)t0 & 0x3FFF : 0, psm, w, h, p, 0, 0, p != NULL ? 0x80000000u | p->gen : 0);
}

/* Native record n drawn as a psm texture of tw x th with palette pal (NULL: the one at cbp in GS memory; palKey
   names it). 0 = not drawable so. */
static GsTex nat_resolve(NatTex *n, uint32_t tbp, uint32_t psm, uint32_t tw, uint32_t th, const NatPal *pal, uint32_t cbp, uint32_t cpsm, uint32_t palKey) {
    uint32_t i, b;
    uint16_t col[256];
    if (n->direct && n->psm == psm && n->tw == tw && n->th == th) { /* as uploaded: no palette */
        n->drawn = gGsFrame;
        if (n->t.mem == NULL && !nat_reload(n)) { return 0; }
        return (GsTex)&n->t;
    }
    if (n->direct && psm == 0x13 && n->viewPak >= 0 && (tbp & 31) == 0) { /* read as 8-bit indices (the light) */
        if ((n = nat_view(n, tw, th)) == NULL) { sNatMiss[2]++; return 0; }
    }
    if (n->direct || n->psm != psm || n->tw != tw || n->th != th) { sNatMiss[2]++; return 0; }
    n->drawn = gGsFrame;
    if (n->t.mem == NULL && !nat_reload(n)) { return 0; }
#ifndef PORT_GS_REF
    if (pal == NULL || pal->addr == 0 || pal->n != (n->bits == 8 ? 256u : 16u)) { sPalMade[1]++; return 0; } /* (a palette tex.pak
                                                                         does not have: no GS memory to read it from) */
#endif
    if (n->bits == 4 && n->bank >= 0 && sBank[n->bank].key == palKey && sBank[n->bank].used) { /* the common case */
        sBank[n->bank].last = gGsFrame;
        return (GsTex)&n->t;
    }
    if (n->bits == 8) { /* the copy with these colours, or one no frame has drawn for two, or a new one */
        NatTex *v, *w = NULL;
        for (v = n;; v = &sNat[v->var - 1]) {
            if (v->tw != n->tw || v->th != n->th) { if (v->var == 0) { break; } continue; } /* (a view of another size) */
            if (v->t.mem != NULL && v->palOk && v->palKey == palKey) { v->palFrame = v->drawn = gGsFrame; return (GsTex)&v->t; }
            if (w == NULL && (!v->palOk || v->palFrame + 2 <= gGsFrame)) { w = v; }
            if (v->var == 0) { break; }
        }
        if (w == NULL && (w = nat_copy(n)) == NULL) { w = n; sNatSteal++; }
        n = w;
        n->drawn = gGsFrame;
        if (n->t.mem == NULL && !nat_reload(n)) { return 0; }
    }
    if (pal != NULL && (pal->addr == 0 || pal->n != (n->bits == 8 ? 256u : 16u))) { pal = NULL; }
    sPalMade[pal == NULL]++;
    for (i = 0; i < (n->bits == 8 ? 256u : 16u); i++) { /* the palette, in the order the indices count: the pack's, or
                                                         read out of GS memory (as gs_draw.c) */
        uint32_t c = n->bits == 8 ? (i & 0xE7) | ((i & 8) << 1) | ((i & 0x10) >> 1) : i;
#ifdef PORT_GS_REF
        col[i] = argb4444(Gs_Expand(pal != NULL ? pal->col[i] : n->bits == 8 ? Gs_VramRead(cbp, 1, cpsm, c & 15, c >> 4) : Gs_VramRead(cbp, 1, cpsm, c & 7, c >> 3), cpsm));
#else
        col[i] = argb4444(Gs_Expand(pal->col[i], cpsm));
        (void)c; (void)cbp;
#endif
    }
    n->palKey = palKey;
    n->palOk = 1;
    n->palFrame = gGsFrame;
    if (n->bits == 8) { /* the codebook: each entry four texels of one colour */
        uint16_t book[1024];
        for (i = 0; i < 256; i++) { book[i * 4] = book[i * 4 + 1] = book[i * 4 + 2] = book[i * 4 + 3] = col[i]; }
        pvr_txr_load(book, n->t.mem, 2048);
        return (GsTex)&n->t;
    }
    for (b = 0; b < 64 && !(sBank[b].used && sBank[b].key == palKey); b++) {} /* the bank with these colours */
    if (b == 64) { /* or one no frame has used for two, or else the one used longest ago */
        uint32_t best = 0;
        for (b = 0; b < 64; b++) {
            if (!sBank[b].used) { best = b; break; }
            if (sBank[b].last < sBank[best].last) { best = b; }
        }
        b = best;
        if (sBank[b].used && sBank[b].last + 2 > gGsFrame) { sNatSteal++; }
        sBank[b].used = 1;
        sBank[b].key = palKey;
        for (i = 0; i < 16; i++) { pvr_set_pal_entry(b * 16 + i, col[i]); }
    }
    sBank[b].last = gGsFrame;
    n->bank = (int)b;
    n->t.fmt = PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_4BPP_PAL(b);
    return (GsTex)&n->t;
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
    tex_handle,
    spill,
};
#endif

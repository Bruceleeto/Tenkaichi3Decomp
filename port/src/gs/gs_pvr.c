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
 * State of it: what is drawn into the shown frame buffer, in the game's order (all in the translucent list,
 * which keeps submission order with autosort off): the 2D primitives (menus, HUD, text: the GS-pixel vertices of
 * gsVerts) and the vertex programs' draws (3D), whose arithmetic (shaders/vu0, vu4, vu6.vert) is done here on the
 * CPU, clipped against the eye and the depth range, depth as 1 / w. Textures arrive decoded to RGBA8 and are kept
 * as ARGB4444 in PVR memory, resampled to a power of two from 8 to 1024. Not drawn yet: lines and points, draws
 * into other frame buffers, draws that sample a frame buffer, the native effects, the destination alpha test.
 */
#ifdef BT3_PVR
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dc/pvr.h>
#include "pvr_host.h"
#include "gs_internal.h"
#include "gs_draw.h"
#include "ui.h"

static SDL_Window *sWindow;

/* What a scene may send: the vertex buffer pvr_init was given, less a margin (the background polygon, and the
   TA's own layout of a polygon being a little larger than what is sent). kos_pvr does not stop at its end: what
   goes past it lands on the frame buffers and the textures. So a frame with more than this loses triangles. */
#define VERTEX_BUF_BYTES (1536 * 1024)
#define SCENE_BUDGET (VERTEX_BUF_BYTES - 128 * 1024)
static uint32_t sSent, sTris, sDropped; /* this scene: bytes sent, triangles sent, triangles left out */

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
        t->mem = pvr_mem_malloc(t->bytes);
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
        mem = pvr_mem_malloc(bytes);
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
        t->mem = pvr_mem_malloc(t->bytes);
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
    if (t->mem != NULL) { pvr_mem_free(t->mem); sTexBytes -= t->bytes; sTexLive--; }
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
        VERTEX_BUF_BYTES, /* vertex buffer: a demo fight frame sends at most about 1.1 MB (as strips); each byte here costs two of the texture heap */
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


static void put_vertex(const ClipV *c, int last) {
    pvr_vertex_t pv;
    pv.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    pv.x = c->x;
    pv.y = c->y;
    pv.z = c->z;
    pv.u = c->u;
    pv.v = c->v;
    pv.argb = (uint32_t)(c->c[3] + 0.5f) << 24 | (uint32_t)(c->c[0] + 0.5f) << 16 | (uint32_t)(c->c[1] + 0.5f) << 8 | (uint32_t)(c->c[2] + 0.5f);
    pv.oargb = 0;
    pvr_prim(&pv, sizeof(pv));
}

/* A triangle (or what is left of it) in screen pixels, clipped against the scissor rectangle and sent as a fan. */
static void emit_poly(ClipV *a, int m, float x0, float y0, float x1, float y1) {
    ClipV b[16];
    int j, k, inside = 1;

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

/* A vertex-program triangle: clipped in clip space against the eye (w > 0) and a guard band of GUARD pixels around
   the screen (as a GPU does: a vertex just in front of the eye would otherwise land millions of pixels away, past
   what the PVR's edge arithmetic holds), then projected to screen pixels, then the scissor. Depth is clamped, not
   clipped: the PC back ends turn the GPU's depth clipping off for the same reason (the sky and far scenery lie
   beyond the GS's depth range and are drawn all the same). */
#define GUARD 1024.0f
/* A clip-space vertex in screen pixels, the PVR's depth and colour. */
static void vu_project(const ClipH *a, const Vu0Uniform *U, ClipV *o) {
    const float sx = 640.0f / 512.0f, sy = 480.0f / 448.0f;
    float zmax = U->misc[2] > 0.0f ? U->misc[2] : -U->misc[2], rw = 1.0f / a->s[3];
    int k;
    o->x = (a->s[0] * rw - U->misc[0]) * sx;
    o->y = (a->s[1] * rw - U->misc[1]) * sy;
    o->z = PVR_Z(zmax > 0.0f ? clampf(a->s[2] * rw / zmax, 0.0f, 1.0f) : rw);
    o->u = a->u;
    o->v = a->v;
    for (k = 0; k < 4; k++) { o->c[k] = (float)gs_unit((uint32_t)clampf(a->c[k], 0.0f, 255.0f)); }
}

static void vu_triangle(int vu, const Vu0Uniform *U, const uint32_t *idx, float x0, float y0, float x1, float y1) {
    const float sx = 640.0f / 512.0f, sy = 480.0f / 448.0f;
    ClipH a[16], b[16];
    ClipV out[16];
    int j, m = 3;
    const float eye[5] = {0.0f, 0.0f, 0.0f, 1.0f, -1e-3f};
    /* screen x = (s.x / w - misc.x) * sx within [-GUARD, 640 + GUARD], y alike */
    const float left[5] = {1.0f, 0.0f, 0.0f, -(U->misc[0] - GUARD / sx), 0.0f};
    const float right[5] = {-1.0f, 0.0f, 0.0f, U->misc[0] + (640.0f + GUARD) / sx, 0.0f};
    const float top[5] = {0.0f, 1.0f, 0.0f, -(U->misc[1] - GUARD / sy), 0.0f};
    const float bottom[5] = {0.0f, -1.0f, 0.0f, U->misc[1] + (480.0f + GUARD) / sy, 0.0f};

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
    return o->x >= x0 && o->x <= x1 && o->y >= y0 && o->y <= y1;
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
   scissor by clipping the triangles here; the destination alpha test not yet (those draws are left out). */
static void draw_list(int shown) {
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
        if (d->native || d->target != shown || d->pipeline < 0 || d->pipeline >= sPipeCount) { continue; }
        if (getenv("BT3_PVR_SKIP") != NULL) { /* BT3_PVR_SKIP=<lo>-<hi>: those draws left out (finding a wrong one) */
            unsigned lo = 0, hi = 0;
            if (sscanf(getenv("BT3_PVR_SKIP"), "%u-%u", &lo, &hi) == 2 && n >= lo && n <= hi) { continue; }
        }
        key = sPipeKey[d->pipeline];
        if (((key >> 10) & 3) == 0 || ((key >> 22) & 1)) { continue; } /* depth test NEVER; a table pass */
        if (((key >> 13) & 3) != 0) { continue; }  /* lines, points */
        if ((key >> 16 & 7) == 0) { continue; }    /* writes no colour (alpha or depth only) */
        if (d->mode[0] == 2) { continue; }          /* samples a frame buffer */
        if (d->mode[3] == 1) { continue; }          /* alpha test NEVER (with KEEP: nothing is written) */
        if (d->misc[1] != 0.0f) { continue; }       /* destination alpha test: TODO (the grass on the stage ground) */
        t = d->mode[0] != 0 ? (const PvrTex *)d->tex : NULL;
        if (d->vu && getenv("BT3_PVR_NOTEX") != NULL) { t = NULL; } /* 3D in its colours only */
        if (t != NULL && t->mem != NULL) {
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, (int)t->fmt, (int)t->pw, (int)t->ph, t->base,
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
        {   /* the GS's depth test (bigger is nearer, as the PVR's) and depth write, 2D and 3D alike */
            int ztst = (int)((key >> 10) & 3);
            cxt.depth.comparison = ztst == 1 ? PVR_DEPTHCMP_ALWAYS : ztst == 2 ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_GREATER;
            cxt.depth.write = ((key >> 12) & 1) ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE; /* (KOS's: 0 = write) */
        }
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
        if (sSent + sizeof(hdr) + 3 * sizeof(pvr_vertex_t) > SCENE_BUDGET) {
            sDropped += d->count / 3;
            continue;
        }
        sSent += sizeof(hdr);
        pvr_prim(&hdr, sizeof(hdr));
        {
            /* the scissor (x, y, width, height at SCALE) in screen pixels */
            float x0 = (float)d->scissor[0] / (float)SCALE * sx, y0 = (float)d->scissor[1] / (float)SCALE * sy;
            float x1 = (float)(d->scissor[0] + d->scissor[2]) / (float)SCALE * sx, y1 = (float)(d->scissor[1] + d->scissor[3]) / (float)SCALE * sy;
            if (d->vu) {
                vu_draw((int)((key >> 20) & 3), &gsVuUni[d->uniform], &gsVuIdx[d->first], d->count, x0, y0, x1, y1);
                continue;
            }
            for (k = 0; k + 2 < d->count; k += 3) {
                ClipV a[12];
                int j;
                for (j = 0; j < 3; j++) {
                    const Vtx *v = &gsVerts[d->first + k + j];
                    float q = v->q != 0.0f ? v->q : 1.0f;
                    a[j].x = v->x * sx;
                    a[j].y = v->y * sy;
                    a[j].z = PVR_Z(clampf(v->z, 0.0f, 1.0f));
                    a[j].u = t != NULL ? v->s / q : 0.0f;
                    a[j].v = t != NULL ? v->t / q : 0.0f;
                    a[j].c[0] = (float)gs_unit(v->r);
                    a[j].c[1] = (float)gs_unit(v->g);
                    a[j].c[2] = (float)gs_unit(v->b);
                    a[j].c[3] = (float)gs_unit(v->a);
                }
                emit_poly(a, 3, x0, y0, x1, y1);
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
    sSent = sTris = sDropped = 0;
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    if (shown >= 0) { draw_list(shown); }
    pvr_list_finish();
    pvr_scene_finish(); /* renders, and gpu_pvr shows the picture (kos_pvr_present) */
    {   /* BT3_SHOT=<n>: every n-th frame of the game to port/build/shots/pvr_NNNNN.ppm (the other back ends' gpu_*) */
        static int every = -1;
        if (every < 0) { every = getenv("BT3_SHOT") != NULL ? atoi(getenv("BT3_SHOT")) : 0; }
        if (every > 0 && gGsFrame % (unsigned)every == 0) {
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
    {
        static uint32_t peak;
        if (sSent > peak) { peak = sSent; }
        if (sDropped != 0 || (getenv("BT3_GS_VERBOSE") != NULL && gGsFrame % 30 == 0)) {
            static unsigned told;
            if (getenv("BT3_GS_VERBOSE") != NULL || told++ < 10) {
                fprintf(stderr, "pvr: frame %u: %u triangles sent (%u KB, the most so far %u KB of %u)%s\n", gGsFrame, sTris, sSent >> 10, peak >> 10,
                        (unsigned)(SCENE_BUDGET >> 10), sDropped ? ", some left out: the vertex buffer is full" : "");
            }
        }
    }
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

/*
 * The VU1 programs as C (port/include/vu1_native.h). On the PS2 the game hands VU1 a block of constants and the DMA
 * chain of a model or a stage mesh; the chain unpacks one batch of vertices after another into VU memory and starts
 * the program on each (MSCNT), which transforms them and sends the GS a GIF packet of triangle strips. Here the
 * chain is walked where it lies, each batch goes through the same arithmetic in C, and the triangles the GS would
 * have drawn go to the renderer as polygons (include/port/rnd.h): no VU memory, no GS packet.
 *
 * The listings this follows are the programs in src/vu1/ (.vsm; docs/systems/vu1/). The arithmetic is done in the same
 * order as the programs do it, the conversions to the GS's integers as they do (ftoi4 / ftoi0, a 24-bit depth), so
 * a corner lands on the GS's sixteenth of a pixel; a division by zero and an overflow give the largest float, as
 * the unit does. The chains' shape is the one the offline audit found in every asset (port/ps2_offline_audit.txt
 * section 2): one "ret" tag, UNPACK V4-32 of a header at TOPS + 0 and of the vertices behind it, MSCNT.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../include/port/rnd.h"
#include "vu1_native.h"
#include "port_addr.h"

extern uint32_t *gDmaCur;                                                /* src/sys/dma.c: the display list's cursor */
extern int Gs_Defers(void);                                              /* gs/gs_walk.c */
extern void *Gs_DeferPush(uint32_t bytes, void (*fn)(const void *), uint32_t at);
extern uint32_t Gs_DeferGen(void);

/* ---------------------------------------------------------------------------------------------- arithmetic */

#define VU_MAX 3.4028235e38f

/* An overflowed float: the largest of its sign, as the unit gives it, instead of an infinity. */
#if defined(__DREAMCAST__) || defined(BT3_DCSIM)
#define VU1_DIRECT 1 /* the strips made straight into the PVR's vertices (Rnd_Direct) */
#else
#define VU1_DIRECT 0
#endif

static inline float vu_clamp(float v) {
    union {
        float f;
        uint32_t u;
    } x;
#if VU1_DIRECT
    if (__builtin_expect(__builtin_fabsf(v) < __builtin_inff(), 1)) {
        return v; /* (a float compare: the SH4 moves a float to an integer register only through memory; < infinity,
                     the same as <= FLT_MAX for every value, NaN too, is one fcmp/gt where <= is fcmp/gt and fcmp/eq) */
    }
#endif
    x.f = v;
    if ((x.u & 0x7F800000u) == 0x7F800000u) {
        x.u = (x.u & 0x80000000u) | 0x7F7FFFFFu;
    }
    return x.f;
}

/* DIV: n / d; by zero the largest float of n's sign. */
static float vu_div(float n, float d) {
    if (d != 0.0f) {
        return vu_clamp(n / d);
    }
    return n < 0.0f ? -VU_MAX : VU_MAX;
}

/* FTOI0 / FTOI4: to an integer (times 16 for ftoi4), toward zero, saturated. */
static int32_t vu_ftoi(float v, float scale) {
    float x = vu_clamp(v * scale);
    if (x >= 2147483648.0f) {
        return 0x7FFFFFFF;
    }
    if (x <= -2147483648.0f) {
        return (int32_t)0x80000000u;
    }
    return (int32_t)x;
}

/* o = m * (x, y, z, w) for a matrix of four column quadwords (c0 * x + c1 * y + c2 * z + c3 * w), summed in the
   order of the programs' mula / madda / madd. */
#if defined(__DREAMCAST__) || defined(BT3_DCSIM)
/* The Dreamcast: the SH4's ftrv, the matrix in XMTRX (sh4zam); its sum is the same order, at the instruction's own
   precision, and only the result is held to the largest float (an overflow inside the sum is not). make dcsim: the
   instruction modelled bit for bit (port/third_party/sh4_fpu_approx). */
#ifdef BT3_DCSIM
#include "../third_party/sh4_fpu_approx/sh4_fpu_approx.h"
static void mtx_apply(float o[4], const float *m, float x, float y, float z, float w) {
    uint32_t mx[16], fv[4];
    float v[4] = {x, y, z, w};
    int i;
    memcpy(mx, m, 64);
    memcpy(fv, v, 16);
    sh4_ftrv(mx, fv, 0);
    memcpy(v, fv, 16);
    for (i = 0; i < 4; i++) {
        o[i] = vu_clamp(v[i]);
    }
}
#else
#include <sh4zam/shz_sh4zam.h>
static void mtx_apply(float o[4], const float *m, float x, float y, float z, float w) {
    shz_vec4_t r;
    shz_xmtrx_load_unaligned_4x4(m);
    r = shz_xmtrx_transform_vec4(shz_vec4_init(x, y, z, w));
    o[0] = vu_clamp(r.x);
    o[1] = vu_clamp(r.y);
    o[2] = vu_clamp(r.z);
    o[3] = vu_clamp(r.w);
}
#endif
#else
static void mtx_apply(float o[4], const float *m, float x, float y, float z, float w) {
    int i;
    for (i = 0; i < 4; i++) {
        float a = m[i] * x;
        a = vu_clamp(a + m[4 + i] * y);
        a = vu_clamp(a + m[8 + i] * z);
        o[i] = vu_clamp(a + m[12 + i] * w);
    }
}
#endif

/* o[i] = m * in[i] for n points: on the Dreamcast the matrix loaded into XMTRX once for all of them (the same sums
   as mtx_apply's, point by point) */
static void mtx_run(float (*o)[4], const float *m, const float (*in)[4], uint32_t n) {
    uint32_t i;
#if defined(__DREAMCAST__) && !defined(BT3_DCSIM)
    shz_xmtrx_load_unaligned_4x4(m);
    for (i = 0; i < n; i++) {
        shz_vec4_t r = shz_xmtrx_transform_vec4(shz_vec4_init(in[i][0], in[i][1], in[i][2], in[i][3]));
        float sum = r.x + r.y + r.z + r.w;
        if (__builtin_expect(sum - sum == 0.0f, 1)) { /* (all four finite: an infinity or a NaN, or a sum too big,
                                                         makes it a NaN; then each clamped, the same values) */
            o[i][0] = r.x;
            o[i][1] = r.y;
            o[i][2] = r.z;
            o[i][3] = r.w;
            continue;
        }
        o[i][0] = vu_clamp(r.x);
        o[i][1] = vu_clamp(r.y);
        o[i][2] = vu_clamp(r.z);
        o[i][3] = vu_clamp(r.w);
    }
#else
    for (i = 0; i < n; i++) {
        mtx_apply(o[i], m, in[i][0], in[i][1], in[i][2], in[i][3]);
    }
#endif
}

/* CLIP: which of the six planes x, y, z = +|w| / -|w| the point is outside (bit 0 x > +|w|, 1 x < -|w|, 2 y > +|w|,
   3 y < -|w|, 4 z > +|w|, 5 z < -|w|). */
static unsigned clip_flags(const float c[4]) {
    float w = c[3] < 0.0f ? -c[3] : c[3];
    return (unsigned)(c[0] > w) | (unsigned)(c[0] < -w) << 1 | (unsigned)(c[1] > w) << 2 | (unsigned)(c[1] < -w) << 3 |
           (unsigned)(c[2] > w) << 4 | (unsigned)(c[2] < -w) << 5;
}

/* A corner as the program stores it for the GS (XYZF2 from the screen position already divided, ST and Q, RGBAQ
   from the colour's integers) and as the GS reads that: x, y 12.4, the depth from bit 4 of the third word. */
extern unsigned gStatVtx; /* gs/gs_walk.c (the fps line) */
static void put_vtx(RndVtx *o, int ctx, const float scr[4], float s, float t, float q, const int32_t col[4]) {
    gStatVtx++;
    uint32_t z = ((uint32_t)vu_ftoi(scr[2], 16.0f) >> 4) & 0xFFFFFF;
    uint32_t rgba = ((uint32_t)col[0] & 0xFF) | ((uint32_t)col[1] & 0xFF) << 8 | ((uint32_t)col[2] & 0xFF) << 16 |
                    ((uint32_t)col[3] & 0xFF) << 24;
    Rnd_Vtx(o, ctx, vu_ftoi(scr[0], 16.0f), vu_ftoi(scr[1], 16.0f), z, s, t, q, rgba);
}

/* ---------------------------------------------------------------------------------------------- the chains */

/* Words copied as words: the SH-4 loads nothing unaligned, so gcc makes a memcpy of 8 bytes from a uint32_t * (or
   of floats) a call to newlib's, byte by byte at the end; these are loads and stores (profiled: 15% of a frame). */
static inline uint64_t ld64(const uint32_t *p) { return p[0] | (uint64_t)p[1] << 32; }
static inline void cp32(void *d, const void *s, unsigned n) { /* n words */
    uint32_t *o = (uint32_t *)d;
    const uint32_t *i = (const uint32_t *)s;
    unsigned j;
    for (j = 0; j < n; j++) { o[j] = i[j]; }
}

/* A GIF tag of a strip or fan as the programs send them: PRIM from the tag (PRE), packed, the registers ST, RGBAQ,
   XYZF2, the end of the packet (EOP) where eop asks for it, the type asked for, texture coordinates as s t q and
   no fog (what a record can draw). */
static int tag_ok(const uint32_t *tag, unsigned type, int eop) {
    uint64_t lo, hi, prim;
    lo = ld64(tag);
    hi = ld64(tag + 2);
    prim = (lo >> 47) & 0x7FF;
    return (!eop || ((lo >> 15) & 1)) && ((lo >> 46) & 1) && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 3 && (hi & 0xFFF) == 0x412 &&
           (prim & 7) == type && !((prim >> 5) & 1) && !((prim >> 8) & 1) && (lo & 0x7FFF) != 0;
}

/* The PRIM a tag carries, as the GS draws by it (with PRMODECONT 0 the attributes are PRMODE's: Rnd_Prim). */
static uint32_t tag_prim(const uint32_t *tag) {
    uint64_t lo;
    lo = ld64(tag);
    return Rnd_Prim((uint32_t)(lo >> 47) & 0x7FF);
}

/* What MSCNT finds at TOP: the header quadwords unpacked to TOPS + 0 (nh) and the vertices unpacked behind them (nv
   quadwords; 0 when none were). 0 = a batch the program cannot take as it is (the chain is not drawn). */
typedef int (*BatchFn)(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv);

/* Which check refused a chain (its line in this file; a refused state's RND_BAD_ << 16): what Port_Lost reports. */
static uint32_t sRefused;

static int refuse(uint32_t why) {
    sRefused = why;
    return 0;
}

/* Walks the VIF codes of a chain; fn for each batch. 0 = a chain of another shape (or a batch fn refused). */
static int walk(const void *chain, BatchFn fn, void *arg) {
    const uint32_t *t = (const uint32_t *)chain, *w, *end, *hdr = NULL, *vtx = NULL;
    uint32_t nh = 0, nv = 0;

    sRefused = 0; /* (0: a check of the batch's own function) */
    if (t == NULL || ((t[0] >> 28) & 7) != 6) { /* one "ret" tag: its VIF codes, then its quadwords */
        return refuse(__LINE__);
    }
    w = t + 2;
    end = t + 4 + (t[0] & 0xFFFF) * 4;
    while (w < end) {
        uint32_t code = *w++, imm = code & 0xFFFF, num;
        switch ((code >> 24) & 0x7F) {
        case 0x00: case 0x10: case 0x11: case 0x13: /* NOP, FLUSHE, FLUSH, FLUSHA */
            break;
        case 0x01: /* STCYCL: only the plain 1 / 1 */
            if (imm != 0x0101) {
                return refuse(__LINE__);
            }
            break;
        case 0x17: /* MSCNT: the program runs on what was unpacked */
            if (hdr == NULL) {
                return refuse(__LINE__);
            }
            if (!fn(arg, hdr, nh, vtx, nv)) {
                return 0; /* (its reason in sRefused) */
            }
            hdr = vtx = NULL;
            nh = nv = 0;
            break;
        case 0x6C: /* UNPACK V4-32, relative to TOPS: the header at 0, the vertices right behind it */
            num = (code >> 16) & 0xFF;
            num = num != 0 ? num : 256;
            if (!(imm & 0x8000) || num * 4 > (uint32_t)(end - w)) {
                return refuse(__LINE__);
            }
            if ((imm & 0x3FF) == 0) {
                hdr = w;
                nh = num;
                vtx = NULL;
                nv = 0;
            } else if (hdr != NULL && vtx == NULL && (imm & 0x3FF) == nh) {
                vtx = w;
                nv = num;
            } else {
                return refuse(__LINE__);
            }
            w += num * 4;
            break;
        default:
            return refuse(__LINE__);
        }
    }
    return 1;
}

/* A header of size h that came in one unpack with its vertices: split. */
static void split(const uint32_t *hdr, uint32_t *nh, const uint32_t **vtx, uint32_t *nv, uint32_t h) {
    if (*vtx == NULL && *nh > h) {
        *vtx = hdr + h * 4;
        *nv = *nh - h;
        *nh = h;
    }
}

/* The textures a draw made later names (W_NOTE): each TEX0 once, with the handles it has now (Port_TexNote). */
#define NOTE_MAX 16
static RndTexNote sNote[NOTE_MAX];
static unsigned sNoteN, sNoteFull;

/* A draw: made now, or queued to be made when the list is (Vu1Defer, at the end of this file). stat: VU1N_; the
   constants k (nk floats) and k2 (nk2) behind them. */
static void submit(int stat, const void *chain, const float *k, unsigned nk, const float *k2, unsigned nk2);

static void note(uint32_t prim, uint64_t tex0) {
    unsigned i;
    if (!((prim >> 4) & 1) || tex0 == 0) {
        return; /* (not textured: Port_Poly does not look) */
    }
    for (i = 0; i < sNoteN; i++) {
        if (sNote[i].tex0 == tex0) {
            return;
        }
    }
    if (sNoteN == NOTE_MAX) {
        sNoteFull = 1;
        return;
    }
    Port_TexNote(&sNote[sNoteN++], tex0);
}

#if VU1_DIRECT
/* A strip's corners straight into the PVR's (include/port/rnd.h Rnd_Direct): s[i] the screen position as the
   program divided it (window coordinates, not yet the GS's 12.4), (u[i], t[i]) the texture coordinate (s / q),
   col[i * step] the colour's integers (0x80 = 1). 1 = drawn, or nothing to draw; 0 = not taken (put_strip). */
static RndPvrVtx sDirect[256] __attribute__((aligned(32)));

static inline uint32_t direct_unit(int32_t c) {
    c &= 0xFF;
    return c >= 0x80 ? 255u : (uint32_t)c * 2u;
}

static int direct_strip(const float (*s)[4], const float *u, const float *t, const int32_t (*col)[4], uint32_t step,
                        const uint8_t *adc, uint32_t n, uint32_t prim, uint64_t tex0) {
    RndPoly h;
    RndDirect d;
    uint32_t k, tris = 0, verts = 0, run = 0, argb = 0;
    int r;

    for (k = 2; k < n; k++) {
        if (adc[k]) {
            run = 0;
            continue;
        }
        tris++;
        verts += run ? 1 : 3;
        run = 1;
    }
    if (tris == 0) {
        return 1;
    }
    memset(&h, 0, sizeof(h));
    h.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
    h.st.flags = RND_VU;
    if ((r = Rnd_Direct(&d, &h, (int)(prim >> 9) & 1, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1, tris, verts)) != 1) {
        return r == 2;
    }
    for (k = 0; k < n; k++) {
        RndPvrVtx *o = &sDirect[k];
        float z = s[k][2] * d.zk + d.zo;
        if (k == 0 || step) {
            const int32_t *c = col[k * step];
            argb = (d.fixa >= 0 ? (uint32_t)d.fixa : direct_unit(c[3])) << 24 | direct_unit(c[0]) << 16 |
                   direct_unit(c[1]) << 8 | direct_unit(c[2]);
        }
        o->x = (s[k][0] - d.xo) * d.sx;
        o->y = (s[k][1] - d.yo) * d.sy;
        o->z = z < d.z0 ? d.z0 : z > d.z1 ? d.z1 : z;
        o->u = u[k] * d.us;
        o->v = t[k] * d.vs;
        o->argb = argb;
    }
    gStatVtx += n;
    Port_DirectSend(sDirect, adc, n);
    return 1;
}

/* The texture coordinate s / q of a corner whose ST and Q are (s, t, w) times q: as the renderer divides them
   (a Q of 0 taken as 1) */
static inline float direct_div(float a, float w, float q) {
    return w == 1.0f ? a : w != 0.0f ? a / w : a * q;
}
#endif

/* The triangles of a strip of n corners, those whose last corner is marked (ADC) left out; two that follow each
   other as one record of four corners (the GS's strip order: 0 1 2, 1 2 3). The texture: TEX0 tex0, or by the
   handle tex where there is one (0: none). */
static void put_strip(const RndVtx *v, const uint8_t *adc, uint32_t n, uint32_t prim, uint64_t tex0, uint32_t tex) {
    RndPoly h;

    memset(&h, 0, sizeof(h));
    h.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
    h.tex = (prim >> 4) & 1 ? tex : 0;
    h.st.flags = RND_VU;
    Rnd_DrawStrip(&h, v, adc, n, (int)(prim >> 9) & 1, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1);
}

/* ---------------------------------------------------------------------------------------------- program 4 */

/* Program 4 draws the stage's meshes: strips of corners with a colour and a texture coordinate each, transformed,
   divided by w, and every triangle that leaves the clip volume clipped against its six planes and sent as a fan
   (the strip skips it). Constants (Vu1Pkt_LoadProg4): screen matrix, clip matrix, clip -> screen matrix. The chain:
   the object header first (the fans' GIF tag), then batches of a NOP tag and its quadword, the strip's tag, and the
   corners (position, colour as floats, s t q). The texture is the one bound before (StgModel_SetTexture). */
#define P4_MAXV 256 /* corners of one strip taken (the program's buffer holds 72) */
#define P4_POLY 32  /* corners of a clipped polygon (a triangle cut by six planes has at most 9) */

static float sP4[48]; /* VU 0..11: screen, clip, clip -> screen */

void Vu1Native_SetProg4(const void *consts) {
    memcpy(sP4, consts, sizeof(sP4));
}

typedef struct P4Vtx {
    float p[4], c[4], t[4]; /* clip position, colour, texture coordinate */
} P4Vtx;

/* What a walk does with each batch: whether it can be drawn (W_CHECK), its textures' handles noted (W_NOTE: a draw
   made later, Vu1Defer), or drawn (W_DRAW). */
enum { W_CHECK, W_DRAW, W_NOTE };

typedef struct P4Walk {
    int draw;         /* W_ */
    int header;       /* the object header has been seen */
    uint32_t fanPrim; /* its PRIM */
    const float *c;   /* the constants (program 4's sP4, 6's sP6, as they were when the draw was queued) */
} P4Walk;

/* Where edge a -> b crosses the plane comp = sign * w (in clip space, before the divide: so with the colour and
   the texture coordinate in perspective): a + (b - a) * |da / (db - da)|. */
static void p4_cross(P4Vtx *o, const P4Vtx *a, const P4Vtx *b, int comp, float sign) {
    float da = a->p[comp] - a->p[3] * sign, db = b->p[comp] - b->p[3] * sign;
    float t = vu_div(da, db - da);
    int i;
    t = t < 0.0f ? -t : t;
    for (i = 0; i < 4; i++) {
        o->p[i] = vu_clamp(vu_clamp((b->p[i] - a->p[i]) * t) + a->p[i]);
        o->c[i] = vu_clamp(vu_clamp((b->c[i] - a->c[i]) * t) + a->c[i]);
        o->t[i] = vu_clamp(vu_clamp((b->t[i] - a->t[i]) * t) + a->t[i]);
    }
}

/* Outside the plane comp = sign * |w| (the CLIP judgement). */
static int p4_out(const P4Vtx *v, int comp, float sign) {
    float w = v->p[3] < 0.0f ? -v->p[3] : v->p[3];
    return sign < 0.0f ? v->p[comp] < -w : v->p[comp] > w;
}

/* One plane of the clipper (Sutherland-Hodgman): the closed polygon src of n corners into dst, closed again; the
   corners written. */
static int p4_plane(P4Vtx *dst, const P4Vtx *src, int n, int comp, float sign) {
    int i, m = 0;
    for (i = 0; i < n && m + 3 <= P4_POLY; i++) {
        const P4Vtx *a = &src[i], *b = &src[i + 1];
        int ao = p4_out(a, comp, sign), bo = p4_out(b, comp, sign);
        if (!ao) {
            dst[m++] = *a;
            if (bo) {
                p4_cross(&dst[m++], a, b, comp, sign);
            }
        } else if (!bo) {
            p4_cross(&dst[m++], a, b, comp, sign);
        }
    }
    if (m != 0) {
        dst[m] = dst[0];
    }
    return m;
}

/* The triangle of strip corners c0, c1, c2 (clip positions, the colours' integers, texture coordinates) clipped
   and sent as a fan, as clip_triangle / emit_fan do: first grown by 0.5% about its centre (so it overlaps the
   strip's triangles around it), then cut by z = -w, z = +w, x = -w, x = +w, y = -w, y = +w in that order; what is
   left goes through the clip -> screen matrix toScr (program 4's and program 6's clippers are the same). The
   texture as put_strip's. */
static void p4_clip(const float *c0, const float *c1, const float *c2, const int32_t (*col)[4], const float *const *tc,
                    uint32_t prim, uint64_t tex0, uint32_t tex, const float *toScr) {
    static const int comp[6] = {2, 2, 0, 0, 1, 1};
    static const float sign[6] = {-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f};
    static P4Vtx poly[2][P4_POLY + 1];
    const float *cp[3];
    float g[4];
    int i, k, n = 3, ctx = (int)(prim >> 9) & 1;
    RndVtx fan[P4_POLY];
    RndPoly r;

    cp[0] = c0;
    cp[1] = c1;
    cp[2] = c2;
    for (i = 0; i < 4; i++) {
        g[i] = vu_clamp(vu_clamp(vu_clamp(c0[i] + c1[i]) + c2[i]) * 0.333333313465118408203125f); /* (I = 0x3EAAAAAA) */
    }
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 4; i++) {
            poly[0][k].p[i] = vu_clamp(g[i] + vu_clamp(vu_clamp(cp[k][i] - g[i]) * 1.00499999523162841796875f)); /* (I = 0x3F80A3D7) */
            poly[0][k].c[i] = (float)col[k][i];
            poly[0][k].t[i] = tc[k][i];
        }
    }
    poly[0][3] = poly[0][0];
    for (k = 0; k < 6 && n != 0; k++) {
        n = p4_plane(poly[(k + 1) & 1], poly[k & 1], n, comp[k], sign[k]);
    }
    if (n < 3) {
        return;
    }
    for (k = 0; k < n; k++) { /* (after six planes the polygon is in poly[0]) */
        const P4Vtx *v = &poly[0][k];
        float s[4], q;
        int32_t c[4];
        mtx_apply(s, toScr, v->p[0], v->p[1], v->p[2], v->p[3]);
        q = vu_div(1.0f, s[3]);
        for (i = 0; i < 4; i++) {
            s[i] = vu_clamp(s[i] * q);
            c[i] = vu_ftoi(v->c[i], 1.0f);
        }
        put_vtx(&fan[k], ctx, s, vu_clamp(v->t[0] * q), vu_clamp(v->t[1] * q), vu_clamp(v->t[2] * q), c);
    }
    for (k = 2; k < n; k++) { /* the fan's triangles: 0, k - 1, k */
        memset(&r, 0, sizeof(r));
        r.v[0] = fan[0];
        r.v[1] = fan[k - 1];
        r.v[2] = fan[k];
        r.n = 3;
        r.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
        r.tex = (prim >> 4) & 1 ? tex : 0;
        r.st.flags = RND_VU;
        Rnd_Draw(&r, ctx, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1);
    }
}

static int p4_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[P4_MAXV];
    static uint8_t adc[P4_MAXV];
    static float clip[P4_MAXV][4], scr[P4_MAXV][4], qv[P4_MAXV], uv[2][P4_MAXV];
    static int32_t col[P4_MAXV][4];
    P4Walk *wk = (P4Walk *)arg;
    uint32_t prim, n, i, k, flags[3] = {0, 0, 0};
    uint64_t tex0, lo;
    int ctx;

    if (!wk->header) { /* the first MSCNT: the object header, one quadword (the fans' tag) */
        if (nh != 1 || vtx != NULL || !tag_ok(hdr, 5, 1)) {
            return 0;
        }
        wk->fanPrim = tag_prim(hdr);
        wk->header = 1;
        if (wk->draw == W_NOTE) {
            note(wk->fanPrim, Rnd_Tex0((int)(wk->fanPrim >> 9) & 1));
        }
        return wk->draw != W_CHECK || gRndCtxBad[(wk->fanPrim >> 9) & 1] == 0;
    }
    split(hdr, &nh, &vtx, &nv, 3);
    lo = ld64(hdr);
    /* +0 a tag of one NOP register (its quadword +1 is thrown away by the GIF), +2 the strip's tag */
    if (nh != 3 || vtx == NULL || (lo & 0x7FFF) != 1 || ((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 1 || (hdr[2] & 15) != 15 ||
        !tag_ok(hdr + 8, 4, 1)) {
        return 0;
    }
    prim = tag_prim(hdr + 8);
    n = hdr[8] & 0x7FFF;
    ctx = (int)(prim >> 9) & 1;
    if (n > P4_MAXV || n * 3 > nv) {
        return 0;
    }
    if (wk->draw == W_CHECK) {
        return gRndCtxBad[ctx] == 0;
    }
    tex0 = Rnd_Tex0(ctx);
    if (wk->draw == W_NOTE) {
        note(prim, tex0);
        return 1;
    }
    for (i = 0; i < n; i++) { /* (the positions as points: the matrices' runs read them from there) */
        cp32(scr[i], vtx + i * 12, 4);
    }
    mtx_run(clip, wk->c + 16, (const float (*)[4])scr, n);
    mtx_run(scr, wk->c, (const float (*)[4])scr, n);
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position, colour, s t q */
        float *s = scr[i], q;

        q = vu_div(1.0f, s[3]);
        for (k = 0; k < 4; k++) {
            s[k] = vu_clamp(s[k] * q);
            col[i][k] = vu_ftoi(v[4 + k], 1.0f);
        }
        qv[i] = q;
        adc[i] = 0;
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(clip[i]);
        /* a triangle with a corner outside: left out of the strip; clipped and sent as a fan before it, unless all
           three corners are outside one plane */
        if (i >= 2 && (flags[0] | flags[1] | flags[2]) != 0) {
            if ((flags[0] & flags[1] & flags[2]) == 0) {
                const float *tc[3];
                tc[0] = (const float *)(vtx + (i - 2) * 12) + 8;
                tc[1] = (const float *)(vtx + (i - 1) * 12) + 8;
                tc[2] = v + 8;
                p4_clip(clip[i - 2], clip[i - 1], clip[i], (const int32_t(*)[4])&col[i - 2], tc, wk->fanPrim, Rnd_Tex0((int)(wk->fanPrim >> 9) & 1), 0, wk->c + 32);
            }
            adc[i] = 1;
        }
    }
#if VU1_DIRECT
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12);
        uv[0][i] = direct_div(v[8], v[10], qv[i]);
        uv[1][i] = direct_div(v[9], v[10], qv[i]);
    }
    if (direct_strip((const float (*)[4])scr, uv[0], uv[1], (const int32_t (*)[4])col, 1, adc, n, prim, tex0)) {
        return 1;
    }
#endif
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12);
        float q = qv[i];
        put_vtx(&out[i], ctx, scr[i], vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col[i]);
    }
    put_strip(out, adc, n, prim, tex0, 0);
    return 1;
}

unsigned gVu1NativeStat[VU1N_STATS][2];

int Vu1Native_Prog4(const void *chain) {
    P4Walk wk;

    if (!Port_TakesRecords()) {
        return 0;
    }
    memset(&wk, 0, sizeof(wk));
    if (!walk(chain, p4_batch, &wk) || !wk.header) {
        gVu1NativeStat[VU1N_P4][0]++;
        Port_Lost("a VU1 program's chain its native code does not take (VU1N_ program | the refusing check's line << 8)", (uint32_t)VU1N_P4 | sRefused << 8, __builtin_return_address(0));
        return 0;
    }
    gVu1NativeStat[VU1N_P4][1]++;
    submit(VU1N_P4, chain, sP4, 48, NULL, 0);
    return 1;
}

/* ---------------------------------------------------------------------------------------------- programs 0 and 1 */

/* Programs 0 and 1 draw the fighters' model parts (ObjDraw_DrawParts): strips of corners skinned between two bones
   by a weight, in two layers at the same corners, the model's texture through one context and a second texture
   through the other. Program 0's second layer is the shading ramp, looked up by the light on the blended normal;
   program 1's (the fade pass) a texture looked up by the normal turned to face the camera, bound by the constants
   (26) instead of the batch, and a batch is left out when its first normal's w says so. Nothing is clipped: a
   triangle with a corner outside the clip volume is left out (ADC). Constants (ObjDrawPkt from mtxA): bone
   matrices A, B (0..7), their pivots (8, 9), light (10..13), screen (14..17), clip (18..21), the two layers'
   colours (22, 23), program 1's TEX0 (26). A batch: a tag of one A+D register, TEX0 of each layer, the two strips'
   tags, then the corners (position with bone A's weight in w, normal, s t q). */
#define FIG_MAXV 38 /* the corners the program has room for (its output, 6 + 6n quadwords, in 238) */

typedef struct FigWalk {
    int prog, draw;  /* (draw: W_) */
    const float *k;  /* the constants, when drawing */
    const float *sc; /* program 2: its block's screen and clip (VU 11..18, sP2 + 44) */
    int alt;         /* program 2: 2b */
    int lit;         /* program 0: lightA, lightB made (once a draw, as the program's setup before its batches) */
    float lightA[16], lightB[16];
} FigWalk;

/* The A+D tag of one register the batch starts with (no EOP: the strip's tag follows in the same kick). */
static int ad_tag_ok(const uint32_t *tag) {
    uint64_t lo, hi;
    lo = ld64(tag);
    hi = ld64(tag + 2);
    return (lo & 0x7FFF) == 1 && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 1 && (hi & 15) == 14;
}

/* An A+D quadword's value, when it writes TEX0 of context ctx (TEX0_1 0x06, TEX0_2 0x07). */
static int ad_tex0(const uint32_t *ad, int ctx, uint64_t *tex0) {
    *tex0 = ld64(ad);
    return (ad[2] & 0xFF) == (uint32_t)(6 + ctx);
}

static int fig_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[2][FIG_MAXV];
    static uint8_t adc[FIG_MAXV];
    static float pa[FIG_MAXV][4], pb[FIG_MAXV][4], ss[FIG_MAXV][4], cs[FIG_MAXV][4], la[FIG_MAXV][4], lb[FIG_MAXV][4];
    static float qa[FIG_MAXV], ua[2][FIG_MAXV], ta[2][FIG_MAXV]; /* q; each layer's texture coordinate (s / q) */
    FigWalk *wk = (FigWalk *)arg;
    const float *k = wk->k;
    uint32_t prim[2], n, i, c, flags[3] = {0, 0, 0};
    uint64_t tex0[2] = {0, 0};
    int ctx[2];
    float *lightA = wk->lightA, *lightB = wk->lightB;
    int32_t col[2][4];

    split(hdr, &nh, &vtx, &nv, 5);
    if (nh != 5 || vtx == NULL) {
        return refuse(__LINE__);
    }
    if (wk->prog == 1 && (vtx[7] & 0xFFFF) == 0) {
        return 1; /* a batch without the fade layer: the program draws nothing */
    }
    if (!ad_tag_ok(hdr) || !tag_ok(hdr + 12, 4, 0) || !tag_ok(hdr + 16, 4, 1)) {
        return refuse(__LINE__);
    }
    prim[0] = tag_prim(hdr + 12);
    prim[1] = tag_prim(hdr + 16);
    ctx[0] = (int)(prim[0] >> 9) & 1;
    ctx[1] = (int)(prim[1] >> 9) & 1;
    n = hdr[12] & 0x7FFF;
    /* (layer 1's strip has layer 0's corners: its tag must say as many) */
    if (n > FIG_MAXV || n * 3 > nv || (hdr[16] & 0x7FFF) != n || !ad_tex0(hdr + 4, ctx[0], &tex0[0]) ||
        (wk->prog == 0 && !ad_tex0(hdr + 8, ctx[1], &tex0[1]))) {
        return refuse(__LINE__);
    }
    if (wk->draw == W_CHECK) {
        return gRndCtxBad[ctx[0]] == 0 && gRndCtxBad[ctx[1]] == 0 ? 1 : refuse(__LINE__ | (gRndCtxBad[ctx[0]] | gRndCtxBad[ctx[1]]) << 16);
    }
    if (wk->prog == 1) {
        tex0[1] = ld64((const uint32_t *)(k + 104)); /* constant 26: TEX0_2 of the fade texture, as ObjDraw_DrawParts writes it */
    }
    if (wk->draw == W_NOTE) {
        note(prim[0], tex0[0]);
        note(prim[1], tex0[1]);
        return 1;
    }
    for (c = 0; c < 4; c++) {
        col[0][c] = vu_ftoi(k[88 + c], 1.0f);
        col[1][c] = vu_ftoi(k[92 + c], 1.0f);
    }
    if (wk->prog == 0 && !wk->lit) { /* the setup's light * A and light * B: columns 0..2 the product, 3 the light's own */
        wk->lit = 1;
        for (c = 0; c < 3; c++) {
            mtx_apply(lightA + c * 4, k + 40, k[c * 4], k[c * 4 + 1], k[c * 4 + 2], k[c * 4 + 3]);
            mtx_apply(lightB + c * 4, k + 40, k[16 + c * 4], k[16 + c * 4 + 1], k[16 + c * 4 + 2], k[16 + c * 4 + 3]);
        }
        cp32(lightA + 12, k + 52, 4);
        cp32(lightB + 12, k + 52, 4);
    }
    /* each matrix run over the batch's corners at once (mtx_run): the corner relative to bone A's pivot and to B's,
       through A and B, blended by the weight; through the screen and the clip; the normal through the light(s) */
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12);
        pa[i][0] = vu_clamp(v[0] - k[32]);
        pa[i][1] = vu_clamp(v[1] - k[33]);
        pa[i][2] = vu_clamp(v[2] - k[34]);
        pa[i][3] = 1.0f;
        pb[i][0] = vu_clamp(v[0] - k[36]);
        pb[i][1] = vu_clamp(v[1] - k[37]);
        pb[i][2] = vu_clamp(v[2] - k[38]);
        pb[i][3] = 1.0f;
        cp32(cs[i], v + 4, 3); /* (the normal) */
        cs[i][3] = 1.0f;
    }
    if (wk->prog == 0) {
        mtx_run(la, lightA, (const float (*)[4])cs, n);
        mtx_run(lb, lightB, (const float (*)[4])cs, n);
    } else {
        mtx_run(la, k + 40, (const float (*)[4])cs, n);
    }
    mtx_run(pa, k, (const float (*)[4])pa, n);
    mtx_run(pb, k + 16, (const float (*)[4])pb, n);
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12);
        for (c = 0; c < 3; c++) {
            ss[i][c] = vu_clamp(pb[i][c] + vu_clamp(vu_clamp(pa[i][c] - pb[i][c]) * v[3]));
        }
        ss[i][3] = 1.0f; /* (the screen and the clip take the blend's x y z with w 1) */
    }
    mtx_run(cs, k + 72, (const float (*)[4])ss, n);
    mtx_run(ss, k + 56, (const float (*)[4])ss, n);
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position (w: bone A's weight), normal, s t q */
        float *s = ss[i], *cl = cs[i], l[4], q, u, t;

        q = vu_div(1.0f, s[3]);
        if (wk->prog == 0) { /* u = 0.5 + 0.5 * (light * the normal blended by the weight).x */
            l[0] = vu_clamp(lb[i][0] + vu_clamp(vu_clamp(la[i][0] - lb[i][0]) * v[3]));
            u = vu_clamp(vu_clamp(l[0] * 0.5f) + 0.5f);
            t = 0.0f;
        } else { /* (u, v) = 0.5 * (1 + (light * normal).xy) */
            cp32(l, la[i], 4);
            u = vu_clamp(vu_clamp(1.0f + l[0]) * 0.5f);
            t = vu_clamp(vu_clamp(1.0f + l[1]) * 0.5f);
        }
        for (c = 0; c < 4; c++) {
            s[c] = vu_clamp(s[c] * q);
        }
        qa[i] = q;
        ua[1][i] = u;
        ta[1][i] = t;
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(cl);
        adc[i] = (flags[0] | flags[1] | flags[2]) != 0; /* (the first two complete no triangle) */
    }
    for (c = 0; c < 2; c++) { /* the two layers, in their order */
#if VU1_DIRECT
        if (c == 0) {
            for (i = 0; i < n; i++) {
                const float *v = (const float *)(vtx + i * 12);
                ua[0][i] = direct_div(v[8], v[10], qa[i]);
                ta[0][i] = direct_div(v[9], v[10], qa[i]);
            }
        }
        if (direct_strip((const float (*)[4])ss, ua[c], ta[c], (const int32_t (*)[4])col[c], 0, adc, n, prim[c], tex0[c])) {
            continue;
        }
#endif
        for (i = 0; i < n; i++) {
            const float *v = (const float *)(vtx + i * 12);
            float q = qa[i];
            if (c == 0) {
                put_vtx(&out[0][i], ctx[0], ss[i], vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col[0]);
            } else {
                put_vtx(&out[1][i], ctx[1], ss[i], vu_clamp(ua[1][i] * q), vu_clamp(ta[1][i] * q), q, col[1]);
            }
        }
        put_strip(out[c], adc, n, prim[c], tex0[c], 0);
    }
    return 1;
}

static const float *sP2; /* program 2: the constant block of the last Vu1Pkt_LoadProg2 (VU 0..18; 11..18 read from it) */
static int sP2Alt;       /* it loaded 2b */
static int p2_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv);

int Vu1Native_Takes(int prog, const void *chain) {
    FigWalk wk;
    int stat;

    if (!Port_TakesRecords()) {
        return 0;
    }
    if (prog < 0 || prog > 2 || (prog == 2 && sP2 == NULL)) {
        Port_Lost("a VU1 program the native code does not have (its number)", (uint32_t)prog, __builtin_return_address(0));
        return 0;
    }
    memset(&wk, 0, sizeof(wk));
    wk.prog = prog;
    stat = prog == 2 ? VU1N_P2A + sP2Alt : prog;
    if (!walk(chain, prog == 2 ? p2_batch : fig_batch, &wk)) {
        gVu1NativeStat[stat][0]++;
        Port_Lost("a VU1 program's chain its native code does not take (VU1N_ program | the refusing check's line << 8)", (uint32_t)stat | sRefused << 8, __builtin_return_address(0));
        return 0;
    }
    gVu1NativeStat[stat][1]++;
    return 1;
}

#define FIG_K 108 /* the constants programs 0 and 1 read (0..26) */

void Vu1Native_Prog0(const void *chain, const void *consts) {
    submit(VU1N_P0, chain, (const float *)consts, FIG_K, NULL, 0);
}

void Vu1Native_Prog1(const void *chain, const void *consts) {
    submit(VU1N_P1, chain, (const float *)consts, FIG_K, NULL, 0);
}

/* ---------------------------------------------------------------------------------------------- program 2 */

/* Programs 2a and 2b draw a fighter's parts in one flat colour (ObjDraw_DrawParts mode 2): 2a the flat shadow in
   the view (ObjShadow_BeginFlat), 2b the silhouette into the shadow page (ObjShadow_LoadProg). The chains are
   program 0's: of a batch's header the program sends the A+D tag and its TEX0 (+0, +1) and layer 1's strip tag
   (+4), then the corners skinned between the two bones as program 0 does it, in the colour of constant 10, with
   the texture coordinates of the corner. 2a leaves out a triangle with a corner outside the clip volume (ADC), 2b
   draws every one. Constants: 0..10 bones A, B, their pivots, the colour (Vu1Pkt_CallProg2: ObjDrawPkt from mtxA);
   11..14 screen, 15..18 clip (Vu1Pkt_LoadProg2's block, filled by its caller). The shadow passes draw with
   PRMODECONT 0: the strip's attributes are PRMODE's (tag_prim). */
void Vu1Native_SetProg2(int alt, const void *consts) {
    sP2 = (const float *)consts;
    sP2Alt = alt != 0;
}

static int p2_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[FIG_MAXV];
    static uint8_t adc[FIG_MAXV];
    FigWalk *wk = (FigWalk *)arg;
    const float *k = wk->k, *sc = wk->sc, *cm = wk->sc != NULL ? wk->sc + 16 : NULL;
    uint32_t prim, n, i, c, flags[3] = {0, 0, 0};
    uint64_t tex0;
    int ctx;
    int32_t col[4];

    split(hdr, &nh, &vtx, &nv, 5);
    if (nh != 5 || vtx == NULL || !ad_tag_ok(hdr) || !tag_ok(hdr + 16, 4, 1) || ((hdr[6] & 0xFF) != 6 && (hdr[6] & 0xFF) != 7)) {
        return 0;
    }
    prim = tag_prim(hdr + 16);
    ctx = (int)(prim >> 9) & 1;
    n = hdr[16] & 0x7FFF;
    if (n > FIG_MAXV || n * 3 > nv) {
        return 0;
    }
    if (wk->draw == W_CHECK) {
        return gRndCtxBad[ctx] == 0;
    }
    if (!ad_tex0(hdr + 4, ctx, &tex0)) { /* (the A+D writes the other context's TEX0: this one's is the one bound) */
        tex0 = Rnd_Tex0(ctx);
    }
    if (wk->draw == W_NOTE) {
        note(prim, tex0);
        return 1;
    }
    for (c = 0; c < 4; c++) {
        col[c] = vu_ftoi(k[40 + c], 1.0f);
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position (w: bone A's weight), normal (not read), s t q */
        float pa[4], pb[4], p[4], s[4], cl[4], q;

        mtx_apply(pa, k, vu_clamp(v[0] - k[32]), vu_clamp(v[1] - k[33]), vu_clamp(v[2] - k[34]), 1.0f);
        mtx_apply(pb, k + 16, vu_clamp(v[0] - k[36]), vu_clamp(v[1] - k[37]), vu_clamp(v[2] - k[38]), 1.0f);
        for (c = 0; c < 4; c++) {
            p[c] = vu_clamp(pb[c] + vu_clamp(vu_clamp(pa[c] - pb[c]) * v[3]));
        }
        mtx_apply(s, sc, p[0], p[1], p[2], 1.0f);
        mtx_apply(cl, cm, p[0], p[1], p[2], 1.0f);
        q = vu_div(1.0f, s[3]);
        for (c = 0; c < 4; c++) {
            s[c] = vu_clamp(s[c] * q);
        }
        put_vtx(&out[i], ctx, s, vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col);
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(cl);
        adc[i] = !wk->alt && (flags[0] | flags[1] | flags[2]) != 0; /* (2b: none) */
    }
    put_strip(out, adc, n, prim, tex0, 0);
    return 1;
}

void Vu1Native_Prog2(const void *chain, const void *consts) {
    /* (its own 0..10, then the block's screen and clip, 11..18) */
    submit(sP2Alt ? VU1N_P2B : VU1N_P2A, chain, (const float *)consts, 44, sP2 + 44, 32);
}

/* ---------------------------------------------------------------------------------------------- program 6 */

/* Program 6 draws the stage's triangles under a fighter (ObjShadow_Draw) with the shadow page the silhouette was
   drawn into (program 2b): program 4 with the texture coordinates made from the position (the shadow camera's
   matrix and a scale: s, t = (c.xy * scale + 1) / 2), four quadwords a corner (position with a no-draw flag in w,
   the normal, the colour, the slot of s t 1), w taken as 1 in every transform of a corner, and the batch's TEX0
   quadwords not sent. The input is a list of triangles sent as one strip (ObjShadow_BuildPacket): the flag marks
   the two strip triangles that join one to the next, which are left out (ADC); one that the clipper cuts is sent
   as a fan only when its last corner has no flag. Constants (Vu1Pkt_LoadProg6): screen, clip, clip -> screen,
   shadow camera (0..15), the scale (16.x). The chain: the fans' tag first (NLOOP 0), then batches of a TEX0 tag
   and its quadword, the strip's tag, the corners. The texture: the TEX0 bound (ObjGs_AddShadowTexEnv), the page
   it names read as a frame buffer (Rnd_TargetTex) with its alpha (TCC 1), as the renderers draw the packet's
   strips from the page: outside the silhouette the page's alpha is 0 and the alpha test (not equal to 0) leaves
   the ground as it was. (The GS reads a PSMCT24 texel's alpha as TEXA's 0x80; drawn so, RND_TEX_RGB, the ground
   around the shadow came out lighter than the packets draw it.) */
#define P6_MAXV 72 /* corners of one batch (the builder sends at most 54) */

static float sP6[68]; /* VU 0..16 */

void Vu1Native_SetProg6(const void *consts) {
    memcpy(sP6, consts, sizeof(sP6));
}

/* The handle of the frame buffer a TEX0 of the shadow page names: 0 for any other (a texture, or a format or
   function a record cannot take as the GS would draw it). */
static uint32_t p6_target(uint64_t tex0) {
    uint32_t tbp = (uint32_t)tex0 & 0x3FFF, tw = (uint32_t)(tex0 >> 26) & 15, th = (uint32_t)(tex0 >> 30) & 15;
    if (((tex0 >> 20) & 0x3F) != 1 || ((tex0 >> 35) & 3) != 0 || (tbp & 31) != 0 || tw > 9 || th > 9) {
        return 0;
    }
    return Rnd_TargetTex(tbp >> 5, 1 << tw, 1 << th, 1);
}

static int p6_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[P6_MAXV];
    static uint8_t adc[P6_MAXV];
    static float clip[P6_MAXV][4], st[P6_MAXV][4];
    static int32_t col[P6_MAXV][4];
    P4Walk *wk = (P4Walk *)arg;
    uint32_t prim, n, i, k, flags[3] = {0, 0, 0}, tex;
    uint64_t tex0, lo;
    int ctx;

    if (!wk->header) { /* the first MSCNT: the fans' tag (NLOOP 0: the clipper writes it) */
        lo = ld64(hdr);
        if (nh != 1 || vtx != NULL || !((lo >> 15) & 1) || !((lo >> 46) & 1) || ((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 3 ||
            (hdr[2] & 0xFFF) != 0x412 || (tag_prim(hdr) & 0x127) != 5) {
            return 0;
        }
        wk->fanPrim = tag_prim(hdr);
        wk->header = 1;
        if (wk->draw == W_NOTE) {
            note(wk->fanPrim, Rnd_Tex0((int)(wk->fanPrim >> 9) & 1));
        }
        return wk->draw != W_CHECK || gRndCtxBad[(wk->fanPrim >> 9) & 1] == 0;
    }
    lo = ld64(hdr);
    /* +0 a tag of one register (+1 its quadword: neither is sent), +2 the strip's tag; the vertices behind */
    if (nh != 3 || vtx == NULL || (lo & 0x7FFF) != 1 || ((lo >> 58) & 3) != 0 || !tag_ok(hdr + 8, 4, 1)) {
        return 0;
    }
    prim = tag_prim(hdr + 8);
    n = hdr[8] & 0x7FFF;
    ctx = (int)(prim >> 9) & 1;
    if (n > P6_MAXV || n * 4 > nv) {
        return 0;
    }
    if (wk->draw == W_CHECK) {
        return gRndCtxBad[ctx] == 0 && (!((prim >> 4) & 1) || p6_target(Rnd_Tex0(ctx)) != 0) &&
               (!((wk->fanPrim >> 4) & 1) || p6_target(Rnd_Tex0((int)(wk->fanPrim >> 9) & 1)) != 0);
    }
    tex0 = Rnd_Tex0(ctx);
    tex = p6_target(tex0);
    if (wk->draw == W_NOTE) { /* (the shadow page's frame buffer is named by its handle: nothing to note) */
        return 1;
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 16); /* position (w: the flag), normal, colour, s t slot */
        float s[4], c[4], q;
        int16_t sum, flag = (int16_t)(vtx[i * 16 + 3] & 0xFFFF);

        mtx_apply(s, wk->c, v[0], v[1], v[2], 1.0f);
        mtx_apply(clip[i], wk->c + 16, v[0], v[1], v[2], 1.0f);
        mtx_apply(c, wk->c + 48, v[0], v[1], v[2], 1.0f);
        st[i][0] = vu_clamp(vu_clamp(vu_clamp(c[0] * wk->c[64]) + 1.0f) * 0.5f);
        st[i][1] = vu_clamp(vu_clamp(vu_clamp(c[1] * wk->c[64]) + 1.0f) * 0.5f);
        st[i][2] = 1.0f;
        st[i][3] = 0.0f;
        q = vu_div(1.0f, s[3]);
        for (k = 0; k < 4; k++) {
            s[k] = vu_clamp(s[k] * q);
            col[i][k] = vu_ftoi(v[8 + k], 1.0f);
        }
        put_vtx(&out[i], ctx, s, vu_clamp(st[i][0] * q), vu_clamp(st[i][1] * q), q, col[i]);
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(clip[i]);
        /* the program's count of the last three corners outside (the two before the first count -2 each), plus the
           flag: a triangle with a corner outside is clipped and sent as a fan before the strip (unless all three
           are outside one plane, or its last corner has the flag: the fan's corners all ADC then), and left out of
           the strip; one inside is left out when the flag says so */
        sum = (int16_t)((i >= 2 ? flags[0] != 0 : -2) + (i >= 1 ? flags[1] != 0 : -2) + (flags[2] != 0));
        if (sum > 0 && (flags[0] & flags[1] & flags[2]) == 0) {
            if (flag == 0) {
                const float *tc[3];
                tc[0] = st[i - 2];
                tc[1] = st[i - 1];
                tc[2] = st[i];
                p4_clip(clip[i - 2], clip[i - 1], clip[i], (const int32_t(*)[4])&col[i - 2], tc, wk->fanPrim,
                        Rnd_Tex0((int)(wk->fanPrim >> 9) & 1), p6_target(Rnd_Tex0((int)(wk->fanPrim >> 9) & 1)), wk->c + 32);
            }
            adc[i] = 1;
        } else {
            adc[i] = (int16_t)(sum + flag) > 0;
        }
    }
    put_strip(out, adc, n, prim, tex0, tex);
    return 1;
}

int Vu1Native_Prog6(const void *chain) {
    P4Walk wk;

    if (!Port_TakesRecords()) {
        return 0;
    }
    memset(&wk, 0, sizeof(wk));
    if (!walk(chain, p6_batch, &wk) || !wk.header) {
        gVu1NativeStat[VU1N_P6][0]++;
        Port_Lost("a VU1 program's chain its native code does not take (VU1N_ program | the refusing check's line << 8)", (uint32_t)VU1N_P6 | sRefused << 8, __builtin_return_address(0));
        return 0;
    }
    gVu1NativeStat[VU1N_P6][1]++;
    submit(VU1N_P6, chain, sP6, 68, NULL, 0);
    return 1;
}

/* ---------------------------------------------------------------------------------------------- program 7 */

/* Program 7 draws the stage's debris (StgModel_DrawDebris): program 4's batches (the 3-quadword header, a corner
   of position, colour, s t q) without the object header and without the clipper. Each mesh is called with its
   own constants (Vu1Pkt_CallProg7): its matrix L (0..3), screen S (4..7), clip C (8..11), its alpha (12.w); the
   program first makes S L and C L (a column of L at a time), w is taken as 1, the corners' alpha is the mesh's,
   and a triangle is left out (ADC) when one of its corners is outside the clip volume or when its last corner's
   flag (the low half of the position's w) is not 0 (the count plus the flag, as 16 bits, not 0). */
#define P7_MAXV 84 /* corners of one batch (the output at TOP + 255 leaves room for 84) */

typedef struct P7Walk {
    int draw;
    float sl[16], cl[16], alpha;
} P7Walk;

static int p7_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[P7_MAXV];
    static uint8_t adc[P7_MAXV];
    P7Walk *wk = (P7Walk *)arg;
    uint32_t prim, n, i, k, flags[3] = {0, 0, 0};
    uint64_t lo;
    int ctx;

    split(hdr, &nh, &vtx, &nv, 3);
    lo = ld64(hdr);
    /* +0 a tag of one NOP register (its quadword +1 is thrown away by the GIF), +2 the strip's tag */
    if (nh != 3 || vtx == NULL || (lo & 0x7FFF) != 1 || ((lo >> 15) & 1) || ((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 1 ||
        (hdr[2] & 15) != 15 || !tag_ok(hdr + 8, 4, 1)) {
        return 0;
    }
    prim = tag_prim(hdr + 8);
    n = hdr[8] & 0x7FFF;
    ctx = (int)(prim >> 9) & 1;
    if (n > P7_MAXV || n * 3 > nv) {
        return 0;
    }
    if (wk->draw == W_CHECK) {
        return gRndCtxBad[ctx] == 0;
    }
    if (wk->draw == W_NOTE) {
        note(prim, Rnd_Tex0(ctx));
        return 1;
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position (w: the flag), colour, s t q */
        float s[4], c[4], q;
        int32_t col[4];

        mtx_apply(s, wk->sl, v[0], v[1], v[2], 1.0f);
        mtx_apply(c, wk->cl, v[0], v[1], v[2], 1.0f);
        q = vu_div(1.0f, s[3]);
        for (k = 0; k < 4; k++) {
            s[k] = vu_clamp(s[k] * q);
            col[k] = vu_ftoi(k < 3 ? v[4 + k] : wk->alpha, 1.0f);
        }
        put_vtx(&out[i], ctx, s, vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col);
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(c);
        adc[i] = (int16_t)(((flags[0] | flags[1] | flags[2]) != 0) + (int16_t)(vtx[i * 12 + 3] & 0xFFFF)) != 0;
    }
    put_strip(out, adc, n, prim, Rnd_Tex0(ctx), 0);
    return 1;
}

int Vu1Native_Prog7(const void *chain, const void *consts) {
    const float *k = (const float *)consts;
    P7Walk wk;
    int j;

    if (!Port_TakesRecords()) {
        return 0;
    }
    memset(&wk, 0, sizeof(wk));
    if (!walk(chain, p7_batch, &wk)) {
        gVu1NativeStat[VU1N_P7][0]++;
        Port_Lost("a VU1 program's chain its native code does not take (VU1N_ program | the refusing check's line << 8)", (uint32_t)VU1N_P7 | sRefused << 8, __builtin_return_address(0));
        return 0;
    }
    gVu1NativeStat[VU1N_P7][1]++;
    for (j = 0; j < 4; j++) { /* S L, C L: S and C times each column of L */
        mtx_apply(wk.sl + j * 4, k + 16, k[j * 4], k[j * 4 + 1], k[j * 4 + 2], k[j * 4 + 3]);
        mtx_apply(wk.cl + j * 4, k + 32, k[j * 4], k[j * 4 + 1], k[j * 4 + 2], k[j * 4 + 3]);
    }
    wk.alpha = k[51];
    submit(VU1N_P7, chain, wk.sl, 33, NULL, 0); /* (sl, cl, alpha: one after the other) */
    return 1;
}

/* ---------------------------------------------------------------------------------------------- program 8 */

/* Program 8 draws the stage's animated objects (StgModel_DrawAnims, a node at a time: Vu1Node_Draw): program 2a's
   corners on the stage's batches. A batch: a tag of one register, TEX0 (+0; packed TEX0_1, its value +1, the
   node's texture as Vu1Node_OrFlags placed it), the strip's tag (+2), corners of position (w: the weight of
   matrix A), colour, s t q. A corner: A (p - ofsA) and B (p - ofsB) blended by the weight (B + (A - B) w), w taken
   as 1 in each transform, then screen and clip; the colour as it is; a triangle with a corner outside the clip
   volume left out (ADC), nothing clipped. Constants: a node's A (world), B (parent), ofsA, ofsB (0..9,
   Vu1Pkt_CallProg8), screen S and clip C (10..17: the block of Vu1Pkt_LoadProg8, filled by its caller). */
#define P8_MAXV 84 /* corners of one batch (the output at TOP + 255 leaves room for 84) */

static const float *sP8; /* VU 0..17 (0..9 not read: each node's own) */

void Vu1Native_SetProg8(const void *consts) {
    sP8 = (const float *)consts;
}

/* The TEX0 a batch's tag of one register (+0, packed) and its quadword (+1) write for context ctx: the register
   TEX0_1 / TEX0_2 itself, or A+D naming it. 1 = that, 0 = another register (the one bound is drawn by). */
static int hdr_tex0(const uint32_t *hdr, int ctx, uint64_t *tex0) {
    uint32_t reg = hdr[2] & 15;
    if (reg == (uint32_t)(6 + ctx) || (reg == 14 && (hdr[6] & 0xFF) == (uint32_t)(6 + ctx))) {
        *tex0 = ld64(hdr + 4);
        return 1;
    }
    return 0;
}

typedef struct P8Walk {
    int draw;        /* W_ */
    const float *k;  /* the node's constants (VU 0..9) */
    const float *sc; /* screen and clip (VU 10..17, sP8 + 40) */
} P8Walk;

static int p8_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[P8_MAXV];
    static uint8_t adc[P8_MAXV];
    P8Walk *wk = (P8Walk *)arg;
    const float *k = wk->k, *sc = wk->sc, *cm = wk->sc + 16;
    uint32_t prim, n, i, c, flags[3] = {0, 0, 0};
    uint64_t tex0, lo;
    int ctx;

    split(hdr, &nh, &vtx, &nv, 3);
    lo = ld64(hdr);
    /* +0 a tag of one register, no end of packet (+1 its value), +2 the strip's tag */
    if (nh != 3 || vtx == NULL || (lo & 0x7FFF) != 1 || ((lo >> 15) & 1) || ((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 1 ||
        !tag_ok(hdr + 8, 4, 1)) {
        return 0;
    }
    prim = tag_prim(hdr + 8);
    n = hdr[8] & 0x7FFF;
    ctx = (int)(prim >> 9) & 1;
    if (n > P8_MAXV || n * 3 > nv) {
        return 0;
    }
    if (wk->draw == W_CHECK) {
        /* the register written: a TEX0 (of either context), or one that changes nothing drawn (NOP) */
        return gRndCtxBad[ctx] == 0 && (hdr_tex0(hdr, 0, &tex0) || hdr_tex0(hdr, 1, &tex0) || (hdr[2] & 15) == 15);
    }
    if (!hdr_tex0(hdr, ctx, &tex0)) { /* (the other context's TEX0: this one's is the one bound) */
        tex0 = Rnd_Tex0(ctx);
    }
    if (wk->draw == W_NOTE) {
        note(prim, tex0);
        return 1;
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position (w: A's weight), colour, s t q */
        float pa[4], pb[4], p[4], s[4], cl[4], q;
        int32_t col[4];

        mtx_apply(pa, k, vu_clamp(v[0] - k[32]), vu_clamp(v[1] - k[33]), vu_clamp(v[2] - k[34]), 1.0f);
        mtx_apply(pb, k + 16, vu_clamp(v[0] - k[36]), vu_clamp(v[1] - k[37]), vu_clamp(v[2] - k[38]), 1.0f);
        for (c = 0; c < 4; c++) {
            p[c] = vu_clamp(pb[c] + vu_clamp(vu_clamp(pa[c] - pb[c]) * v[3]));
        }
        mtx_apply(s, sc, p[0], p[1], p[2], 1.0f);
        mtx_apply(cl, cm, p[0], p[1], p[2], 1.0f);
        q = vu_div(1.0f, s[3]);
        for (c = 0; c < 4; c++) {
            s[c] = vu_clamp(s[c] * q);
            col[c] = vu_ftoi(v[4 + c], 1.0f);
        }
        put_vtx(&out[i], ctx, s, vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col);
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(cl);
        adc[i] = (flags[0] | flags[1] | flags[2]) != 0;
    }
    put_strip(out, adc, n, prim, tex0, 0);
    return 1;
}

int Vu1Native_Prog8(const void *chain, const void *consts) {
    P8Walk wk;

    if (!Port_TakesRecords()) {
        return 0;
    }
    if (sP8 == NULL) {
        Port_Lost("a VU1 program 8 chain before its program was loaded (sP8)", 8, __builtin_return_address(0));
        return 0;
    }
    memset(&wk, 0, sizeof(wk));
    wk.k = (const float *)consts;
    if (!walk(chain, p8_batch, &wk)) {
        gVu1NativeStat[VU1N_P8][0]++;
        Port_Lost("a VU1 program's chain its native code does not take (VU1N_ program | the refusing check's line << 8)", (uint32_t)VU1N_P8 | sRefused << 8, __builtin_return_address(0));
        return 0;
    }
    gVu1NativeStat[VU1N_P8][1]++;
    submit(VU1N_P8, chain, wk.k, 40, sP8 + 40, 32);
    return 1;
}

/* ---------------------------------------------------------------------------------------------- made later */

/* A draw queued with the list (gs/gs_walk.c Gs_DeferPush) and made when the list is, as the PS2's VU1 runs a chain
   when the list reaches it: the chain read then (the model's own data, there until the list is drawn, as the PS2's
   DMA reads it then too), the constants and the followed GS state as they were when it was queued (Vu1Body), the
   textures by the handles they had then (Port_TexNotes). No triangles are kept: a frame's would be some MB. A draw
   whose state and constants are those of one queued before it in the same list (a stage's meshes, all under the
   constants of one Vu1Pkt_LoadProg4) keeps none of its own and names that one's (body): 40 bytes or so instead of
   320 (a block queued stays where it is until the list is drawn). */
typedef struct Vu1Defer {
    const void *chain;
    const void *body; /* NULL: its Vu1Body follows; else the one of the draw it shares it with */
    uint8_t stat;   /* VU1N_ */
    uint8_t notes;  /* RndTexNote at the end */
    uint16_t nk;    /* floats of constants */
} Vu1Defer;

#define DEFER_MAX_K 108
typedef struct Vu1Body {
    RndSnap st;
    float k[DEFER_MAX_K]; /* (nk of them kept) */
} Vu1Body;

#define HDR_BYTES ((sizeof(Vu1Defer) + 7) & ~(size_t)7)
#define BODY_BYTES(nk) ((offsetof(Vu1Body, k) + (nk) * 4 + 7) & ~(size_t)7)

static const Vu1Body *body_of(const Vu1Defer *m) {
    return m->body != NULL ? (const Vu1Body *)m->body : (const Vu1Body *)((const uint8_t *)m + HDR_BYTES);
}

static RndTexNote *notes_of(const Vu1Defer *m) {
    return (RndTexNote *)((uint8_t *)m + HDR_BYTES + (m->body == NULL ? BODY_BYTES(m->nk) : 0));
}

/* The draw being queued, and the last one that kept its own Vu1Body: in which list (Gs_DeferGen), where. */
static union {
    Vu1Defer d;
    uint64_t align[(HDR_BYTES + sizeof(Vu1Body) + NOTE_MAX * sizeof(RndTexNote)) / 8 + 1];
} sStage;
static Vu1Body sBody, sOwner;
static const Vu1Body *sOwnerAt;
static uint32_t sOwnerGen, sOwnerNk;
static int sOwnerStat = -1;

/* The walk of a draw of program stat (VU1N_) with constants k, in mode (W_DRAW, W_NOTE). */
static void run(int stat, const void *chain, const float *k, int mode) {
    switch (stat) {
    case VU1N_P0:
    case VU1N_P1:
    case VU1N_P2A:
    case VU1N_P2B: {
        FigWalk wk;
        memset(&wk, 0, sizeof(wk));
        wk.prog = stat == VU1N_P0 ? 0 : stat == VU1N_P1 ? 1 : 2;
        wk.draw = mode;
        wk.k = k;
        wk.sc = k + 44;
        wk.alt = stat == VU1N_P2B;
        walk(chain, wk.prog == 2 ? p2_batch : fig_batch, &wk);
        break;
    }
    case VU1N_P4:
    case VU1N_P6: {
        P4Walk wk;
        memset(&wk, 0, sizeof(wk));
        wk.draw = mode;
        wk.c = k;
        walk(chain, stat == VU1N_P4 ? p4_batch : p6_batch, &wk);
        break;
    }
    case VU1N_P7: {
        P7Walk wk;
        memset(&wk, 0, sizeof(wk));
        memcpy(wk.sl, k, sizeof(wk.sl));
        memcpy(wk.cl, k + 16, sizeof(wk.cl));
        wk.alpha = k[32];
        wk.draw = mode;
        walk(chain, p7_batch, &wk);
        break;
    }
    case VU1N_P8: {
        P8Walk wk;
        memset(&wk, 0, sizeof(wk));
        wk.draw = mode;
        wk.k = k;
        wk.sc = k + 40;
        walk(chain, p8_batch, &wk);
        break;
    }
    }
}

/* gs/gs_walk.c, where the list is drawn: the draw made, in the state it was queued in. */
static void replay(const void *blob) {
    const Vu1Defer *m = (const Vu1Defer *)blob;
    const Vu1Body *b = body_of(m);
    RndSnap now;

    Rnd_SnapTake(&now);
    Rnd_SnapPut(&b->st);
    Port_TexNotes(notes_of(m), m->notes);
    run(m->stat, m->chain, b->k, W_DRAW);
    Port_TexNotes(NULL, 0);
    Rnd_SnapPut(&now);
}

static void submit(int stat, const void *chain, const float *k, unsigned nk, const float *k2, unsigned nk2) {
    Vu1Defer *m = &sStage.d, *q;
    uint32_t bytes, gen;
    int shared;

    memcpy(sBody.k, k, nk * 4);
    if (nk2 != 0) {
        memcpy(sBody.k + nk, k2, nk2 * 4);
    }
    nk += nk2;
    if (Gs_Defers()) {
        sNoteN = sNoteFull = 0;
        run(stat, chain, sBody.k, W_NOTE);
        if (!sNoteFull) {
            Rnd_SnapTake(&sBody.st);
            gen = Gs_DeferGen();
            shared = sOwnerStat == stat && sOwnerNk == nk && sOwnerGen == gen &&
                     memcmp(&sOwner, &sBody, offsetof(Vu1Body, k) + nk * 4) == 0;
            m->chain = chain;
            m->body = shared ? sOwnerAt : NULL;
            m->stat = (uint8_t)stat;
            m->nk = (uint16_t)nk;
            m->notes = (uint8_t)sNoteN;
            if (!shared) {
                memcpy((uint8_t *)m + HDR_BYTES, &sBody, offsetof(Vu1Body, k) + nk * 4);
            }
            memcpy(notes_of(m), sNote, sNoteN * sizeof(RndTexNote));
            bytes = (uint32_t)((const uint8_t *)(notes_of(m) + sNoteN) - (const uint8_t *)m);
            q = (Vu1Defer *)Gs_DeferPush(bytes, replay, Port_Addr(gDmaCur));
            if (q != NULL) { /* (NULL: no memory, said: made now) */
                memcpy(q, m, bytes);
                if (!shared) {
                    memcpy(&sOwner, &sBody, offsetof(Vu1Body, k) + nk * 4);
                    sOwnerAt = body_of(q);
                    sOwnerStat = stat;
                    sOwnerNk = nk;
                    sOwnerGen = gen;
                }
                return;
            }
        } else {
            static int said;
            if (!said) { /* (drawn right, as records: only the memory the deferral saves is not saved) */
                said = 1;
                fprintf(stderr, "vu1_native: a draw that names more than %d textures, made now (as records), not later: NOTE_MAX\n", NOTE_MAX);
            }
        }
    }
    run(stat, chain, sBody.k, W_DRAW);
}

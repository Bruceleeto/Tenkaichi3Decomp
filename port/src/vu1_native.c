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
#include <stdint.h>
#include <string.h>
#include "../../include/port/rnd.h"
#include "vu1_native.h"

/* ---------------------------------------------------------------------------------------------- arithmetic */

#define VU_MAX 3.4028235e38f

/* An overflowed float: the largest of its sign, as the unit gives it, instead of an infinity. */
static float vu_clamp(float v) {
    union {
        float f;
        uint32_t u;
    } x;
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
static void mtx_apply(float o[4], const float *m, float x, float y, float z, float w) {
    int i;
    for (i = 0; i < 4; i++) {
        float a = m[i] * x;
        a = vu_clamp(a + m[4 + i] * y);
        a = vu_clamp(a + m[8 + i] * z);
        o[i] = vu_clamp(a + m[12 + i] * w);
    }
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
static void put_vtx(RndVtx *o, int ctx, const float scr[4], float s, float t, float q, const int32_t col[4]) {
    uint32_t z = ((uint32_t)vu_ftoi(scr[2], 16.0f) >> 4) & 0xFFFFFF;
    uint32_t rgba = ((uint32_t)col[0] & 0xFF) | ((uint32_t)col[1] & 0xFF) << 8 | ((uint32_t)col[2] & 0xFF) << 16 |
                    ((uint32_t)col[3] & 0xFF) << 24;
    Rnd_Vtx(o, ctx, vu_ftoi(scr[0], 16.0f), vu_ftoi(scr[1], 16.0f), z, s, t, q, rgba);
}

/* ---------------------------------------------------------------------------------------------- the chains */

/* A GIF tag of a strip or fan as the programs send them: PRIM from the tag (PRE), packed, the registers ST, RGBAQ,
   XYZF2, the end of the packet (EOP) where eop asks for it, the type asked for, texture coordinates as s t q and
   no fog (what a record can draw). */
static int tag_ok(const uint32_t *tag, unsigned type, int eop) {
    uint64_t lo, hi, prim;
    memcpy(&lo, tag, 8);
    memcpy(&hi, tag + 2, 8);
    prim = (lo >> 47) & 0x7FF;
    return (!eop || ((lo >> 15) & 1)) && ((lo >> 46) & 1) && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 3 && (hi & 0xFFF) == 0x412 &&
           (prim & 7) == type && !((prim >> 5) & 1) && !((prim >> 8) & 1) && (lo & 0x7FFF) != 0;
}

/* The PRIM a tag carries, as the GS draws by it (with PRMODECONT 0 the attributes are PRMODE's: Rnd_Prim). */
static uint32_t tag_prim(const uint32_t *tag) {
    uint64_t lo;
    memcpy(&lo, tag, 8);
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

/* The triangles of a strip of n corners, those whose last corner is marked (ADC) left out; two that follow each
   other as one record of four corners (the GS's strip order: 0 1 2, 1 2 3). The texture: TEX0 tex0, or by the
   handle tex where there is one (0: none). */
static void put_strip(const RndVtx *v, const uint8_t *adc, uint32_t n, uint32_t prim, uint64_t tex0, uint32_t tex) {
    int ctx = (int)(prim >> 9) & 1;
    uint32_t k = 2;
    RndPoly p;

    while (k < n) {
        if (adc[k]) {
            k++;
            continue;
        }
        memset(&p, 0, sizeof(p));
        if (k + 1 < n && !adc[k + 1]) {
            memcpy(p.v, &v[k - 2], 4 * sizeof(RndVtx));
            p.n = 4;
            k += 2;
        } else {
            memcpy(p.v, &v[k - 2], 3 * sizeof(RndVtx));
            p.n = 3;
            k++;
        }
        p.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
        p.tex = (prim >> 4) & 1 ? tex : 0;
        p.st.flags = RND_VU;
        Rnd_Draw(&p, ctx, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1);
    }
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

typedef struct P4Walk {
    int draw;         /* 0: only whether every batch can be drawn */
    int header;       /* the object header has been seen */
    uint32_t fanPrim; /* its PRIM */
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
    static float clip[P4_MAXV][4];
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
        return wk->draw || gRndCtxBad[(wk->fanPrim >> 9) & 1] == 0;
    }
    split(hdr, &nh, &vtx, &nv, 3);
    memcpy(&lo, hdr, 8);
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
    if (!wk->draw) {
        return gRndCtxBad[ctx] == 0;
    }
    tex0 = Rnd_Tex0(ctx);
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position, colour, s t q */
        float s[4], q;

        mtx_apply(s, sP4, v[0], v[1], v[2], v[3]);
        mtx_apply(clip[i], sP4 + 16, v[0], v[1], v[2], v[3]);
        q = vu_div(1.0f, s[3]);
        for (k = 0; k < 4; k++) {
            s[k] = vu_clamp(s[k] * q);
            col[i][k] = vu_ftoi(v[4 + k], 1.0f);
        }
        put_vtx(&out[i], ctx, s, vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col[i]);
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
                p4_clip(clip[i - 2], clip[i - 1], clip[i], (const int32_t(*)[4])&col[i - 2], tc, wk->fanPrim, Rnd_Tex0((int)(wk->fanPrim >> 9) & 1), 0, sP4 + 32);
            }
            adc[i] = 1;
        }
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
    memset(&wk, 0, sizeof(wk));
    wk.draw = 1;
    walk(chain, p4_batch, &wk);
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
    int prog, draw;
    const float *k; /* the constants, when drawing */
} FigWalk;

/* The A+D tag of one register the batch starts with (no EOP: the strip's tag follows in the same kick). */
static int ad_tag_ok(const uint32_t *tag) {
    uint64_t lo, hi;
    memcpy(&lo, tag, 8);
    memcpy(&hi, tag + 2, 8);
    return (lo & 0x7FFF) == 1 && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 1 && (hi & 15) == 14;
}

/* An A+D quadword's value, when it writes TEX0 of context ctx (TEX0_1 0x06, TEX0_2 0x07). */
static int ad_tex0(const uint32_t *ad, int ctx, uint64_t *tex0) {
    memcpy(tex0, ad, 8);
    return (ad[2] & 0xFF) == (uint32_t)(6 + ctx);
}

static int fig_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[2][FIG_MAXV];
    static uint8_t adc[FIG_MAXV];
    FigWalk *wk = (FigWalk *)arg;
    const float *k = wk->k;
    uint32_t prim[2], n, i, c, flags[3] = {0, 0, 0};
    uint64_t tex0[2] = {0, 0};
    int ctx[2];
    float lightA[16], lightB[16];
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
    if (!wk->draw) {
        return gRndCtxBad[ctx[0]] == 0 && gRndCtxBad[ctx[1]] == 0 ? 1 : refuse(__LINE__ | (gRndCtxBad[ctx[0]] | gRndCtxBad[ctx[1]]) << 16);
    }
    if (wk->prog == 1) {
        memcpy(&tex0[1], k + 104, 8); /* constant 26: TEX0_2 of the fade texture, as ObjDraw_DrawParts writes it */
    }
    for (c = 0; c < 4; c++) {
        col[0][c] = vu_ftoi(k[88 + c], 1.0f);
        col[1][c] = vu_ftoi(k[92 + c], 1.0f);
    }
    if (wk->prog == 0) { /* the setup's light * A and light * B: columns 0..2 the product, 3 the light's own */
        for (c = 0; c < 3; c++) {
            mtx_apply(lightA + c * 4, k + 40, k[c * 4], k[c * 4 + 1], k[c * 4 + 2], k[c * 4 + 3]);
            mtx_apply(lightB + c * 4, k + 40, k[16 + c * 4], k[16 + c * 4 + 1], k[16 + c * 4 + 2], k[16 + c * 4 + 3]);
        }
        memcpy(lightA + 12, k + 52, 16);
        memcpy(lightB + 12, k + 52, 16);
    }
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 12); /* position (w: bone A's weight), normal, s t q */
        float pa[4], pb[4], p[4], s[4], cl[4], l[4], q, u, t;

        mtx_apply(pa, k, vu_clamp(v[0] - k[32]), vu_clamp(v[1] - k[33]), vu_clamp(v[2] - k[34]), 1.0f);
        mtx_apply(pb, k + 16, vu_clamp(v[0] - k[36]), vu_clamp(v[1] - k[37]), vu_clamp(v[2] - k[38]), 1.0f);
        for (c = 0; c < 4; c++) {
            p[c] = vu_clamp(pb[c] + vu_clamp(vu_clamp(pa[c] - pb[c]) * v[3]));
        }
        mtx_apply(s, k + 56, p[0], p[1], p[2], 1.0f);
        mtx_apply(cl, k + 72, p[0], p[1], p[2], 1.0f);
        q = vu_div(1.0f, s[3]);
        if (wk->prog == 0) { /* u = 0.5 + 0.5 * (light * the normal blended by the weight).x */
            float la[4], lb[4];
            mtx_apply(la, lightA, v[4], v[5], v[6], 1.0f);
            mtx_apply(lb, lightB, v[4], v[5], v[6], 1.0f);
            l[0] = vu_clamp(lb[0] + vu_clamp(vu_clamp(la[0] - lb[0]) * v[3]));
            u = vu_clamp(vu_clamp(l[0] * 0.5f) + 0.5f);
            t = 0.0f;
        } else { /* (u, v) = 0.5 * (1 + (light * normal).xy) */
            mtx_apply(l, k + 40, v[4], v[5], v[6], 1.0f);
            u = vu_clamp(vu_clamp(1.0f + l[0]) * 0.5f);
            t = vu_clamp(vu_clamp(1.0f + l[1]) * 0.5f);
        }
        for (c = 0; c < 4; c++) {
            s[c] = vu_clamp(s[c] * q);
        }
        put_vtx(&out[0][i], ctx[0], s, vu_clamp(v[8] * q), vu_clamp(v[9] * q), vu_clamp(v[10] * q), col[0]);
        put_vtx(&out[1][i], ctx[1], s, vu_clamp(u * q), vu_clamp(t * q), q, col[1]);
        flags[0] = flags[1];
        flags[1] = flags[2];
        flags[2] = clip_flags(cl);
        adc[i] = (flags[0] | flags[1] | flags[2]) != 0; /* (the first two complete no triangle) */
    }
    put_strip(out[0], adc, n, prim[0], tex0[0], 0);
    put_strip(out[1], adc, n, prim[1], tex0[1], 0);
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

static void fig_draw(int prog, const void *chain, const void *consts) {
    FigWalk wk;

    memset(&wk, 0, sizeof(wk));
    wk.prog = prog;
    wk.draw = 1;
    wk.k = (const float *)consts;
    walk(chain, fig_batch, &wk);
}

void Vu1Native_Prog0(const void *chain, const void *consts) {
    fig_draw(0, chain, consts);
}

void Vu1Native_Prog1(const void *chain, const void *consts) {
    fig_draw(1, chain, consts);
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
    const float *k = wk->k, *sc = sP2 + 44, *cm = sP2 + 60;
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
    if (!wk->draw) {
        return gRndCtxBad[ctx] == 0;
    }
    if (!ad_tex0(hdr + 4, ctx, &tex0)) { /* (the A+D writes the other context's TEX0: this one's is the one bound) */
        tex0 = Rnd_Tex0(ctx);
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
        adc[i] = !sP2Alt && (flags[0] | flags[1] | flags[2]) != 0; /* (2b: none) */
    }
    put_strip(out, adc, n, prim, tex0, 0);
    return 1;
}

void Vu1Native_Prog2(const void *chain, const void *consts) {
    FigWalk wk;

    memset(&wk, 0, sizeof(wk));
    wk.prog = 2;
    wk.draw = 1;
    wk.k = (const float *)consts;
    walk(chain, p2_batch, &wk);
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
        memcpy(&lo, hdr, 8);
        if (nh != 1 || vtx != NULL || !((lo >> 15) & 1) || !((lo >> 46) & 1) || ((lo >> 58) & 3) != 0 || ((lo >> 60) & 15) != 3 ||
            (hdr[2] & 0xFFF) != 0x412 || (tag_prim(hdr) & 0x127) != 5) {
            return 0;
        }
        wk->fanPrim = tag_prim(hdr);
        wk->header = 1;
        return wk->draw || gRndCtxBad[(wk->fanPrim >> 9) & 1] == 0;
    }
    memcpy(&lo, hdr, 8);
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
    if (!wk->draw) {
        return gRndCtxBad[ctx] == 0 && (!((prim >> 4) & 1) || p6_target(Rnd_Tex0(ctx)) != 0) &&
               (!((wk->fanPrim >> 4) & 1) || p6_target(Rnd_Tex0((int)(wk->fanPrim >> 9) & 1)) != 0);
    }
    tex0 = Rnd_Tex0(ctx);
    tex = p6_target(tex0);
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(vtx + i * 16); /* position (w: the flag), normal, colour, s t slot */
        float s[4], c[4], q;
        int16_t sum, flag = (int16_t)(vtx[i * 16 + 3] & 0xFFFF);

        mtx_apply(s, sP6, v[0], v[1], v[2], 1.0f);
        mtx_apply(clip[i], sP6 + 16, v[0], v[1], v[2], 1.0f);
        mtx_apply(c, sP6 + 48, v[0], v[1], v[2], 1.0f);
        st[i][0] = vu_clamp(vu_clamp(vu_clamp(c[0] * sP6[64]) + 1.0f) * 0.5f);
        st[i][1] = vu_clamp(vu_clamp(vu_clamp(c[1] * sP6[64]) + 1.0f) * 0.5f);
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
                        Rnd_Tex0((int)(wk->fanPrim >> 9) & 1), p6_target(Rnd_Tex0((int)(wk->fanPrim >> 9) & 1)), sP6 + 32);
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
    memset(&wk, 0, sizeof(wk));
    wk.draw = 1;
    walk(chain, p6_batch, &wk);
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
    memcpy(&lo, hdr, 8);
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
    if (!wk->draw) {
        return gRndCtxBad[ctx] == 0;
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
    wk.draw = 1;
    walk(chain, p7_batch, &wk);
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
        memcpy(tex0, hdr + 4, 8);
        return 1;
    }
    return 0;
}

typedef struct P8Walk {
    int draw;
    const float *k;
} P8Walk;

static int p8_batch(void *arg, const uint32_t *hdr, uint32_t nh, const uint32_t *vtx, uint32_t nv) {
    static RndVtx out[P8_MAXV];
    static uint8_t adc[P8_MAXV];
    P8Walk *wk = (P8Walk *)arg;
    const float *k = wk->k, *sc = sP8 + 40, *cm = sP8 + 56;
    uint32_t prim, n, i, c, flags[3] = {0, 0, 0};
    uint64_t tex0, lo;
    int ctx;

    split(hdr, &nh, &vtx, &nv, 3);
    memcpy(&lo, hdr, 8);
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
    if (!wk->draw) {
        /* the register written: a TEX0 (of either context), or one that changes nothing drawn (NOP) */
        return gRndCtxBad[ctx] == 0 && (hdr_tex0(hdr, 0, &tex0) || hdr_tex0(hdr, 1, &tex0) || (hdr[2] & 15) == 15);
    }
    if (!hdr_tex0(hdr, ctx, &tex0)) { /* (the other context's TEX0: this one's is the one bound) */
        tex0 = Rnd_Tex0(ctx);
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
    wk.draw = 1;
    walk(chain, p8_batch, &wk);
    return 1;
}

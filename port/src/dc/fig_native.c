/* The Dreamcast's fighters drawn natively (src/sys/gfxm_d_c.c ObjDraw_DrawParts): no VU1, no VIF and no GIF tags at
   run time. Programs 0 (a part lit: the model's texture, then the shading ramp over it), 1 (the fade pass: the model's
   texture, then the fade texture by the normal facing the camera) and 2 (2a the flat shadow, 2b the silhouette into
   the shadow page) all read a part's one chain, so all three are drawn from it here.
   - A part's chain is read once, as a file, at its first draw, and becomes in its own memory (a corner is 48 bytes
     there, 36 here) its batches: each the raw PRIM of its two strips, the two TEX0 its header writes, and the
     corners (position, bone A's weight, normal, s / q and t / q).
   - The parts ObjDraw_DrawParts draws at once (one of the three programs, its constants as it made them) are one
     draw queued where the display list has it (Gs_DeferPush), as the other draws are: each part's matrices with the
     pivots folded in, its lighting rows, its colours; the textures' handles of then.
   - Made: the batches sorted by their layer's PRIM and TEX0, one polygon header a key; each part's corners skinned in
     the clip volume by two passes of ftrv (clip times bone A, clip times bone B, blended by the weight; the screen
     from the clip volume and the viewport, as vu1_native.c fig_prepare), one reciprocal each; a triangle with a
     corner outside the clip volume (four screens wide, or behind the near plane) left out, as the programs do; the
     strips straight to the TA by store queue, every first layer before any second.
   - Program 0's second layer (the shading ramp, subtracted) is not drawn: the ramp's colour at each corner's u is
     taken out of the first layer's colour instead (ramp_read: its texels and palette read on the CPU), one pass. A
     batch whose ramp or state this does not read keeps its second layer.
   A chain this does not read goes the old way (port/src/vu1_native.c, or the VU1 packet); a strip the PVR path does
   not take (Rnd_Direct 0: the shadow page, a state it lacks) is drawn by Rnd_DrawStrip. */
#if defined(__DREAMCAST__) && !defined(BT3_DCSIM) && !defined(BT3_OLDFIG)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dc/pvr.h>
#include <sh4zam/shz_sh4zam.h>
#include "../../../include/port/rnd.h"
#include "vu1_native.h"
#include "../port_addr.h"

extern uint32_t *gDmaCur; /* src/sys/dma.c: the display list's cursor */
extern int Gs_Defers(void); /* gs/gs_walk.c */
extern void *Gs_DeferPush(uint32_t bytes, void (*fn)(const void *), uint32_t at);
extern int gPortResim, gsSkipFrame; /* gs/gs_internal.h GS_NO_PICTURE */
extern unsigned gStatVtx, gGsFrame;
extern uint64_t Gs_NowNs(void);

/* ---- the chains, read once -------------------------------------------------------------------------------- */

/* A chain once read:
     chain + 0  the DMA tag's first word, as it was
     chain + 4  "NATF" (a ret tag's address word, 0 in the game's chains)
     chain + 8  its batches, its corners
     chain + 16 the batches, each a FigBatch and its corners */
#define FIG_MAGIC 0x4654414Eu
#define FIG_DATA 16
#define FIG_MAXV 38 /* corners of a batch (what the programs have room for) */

typedef struct FigBatch {
    uint32_t tex0[2][2]; /* the TEX0 of each layer its header writes (A+D quadwords +1, +2) */
    uint32_t prim[2];    /* each layer's strip's PRIM, raw (Rnd_Prim when drawn) */
    uint16_t n, fade;    /* corners; the fade pass draws it (its first normal's w) */
    uint8_t reg[2], pad[2]; /* the register each A+D quadword writes */
} FigBatch; /* 32 bytes */

typedef struct FigVtx {
    float x, y, z, w; /* w: bone A's weight */
    float nx, ny, nz;
    float u, v;
} FigVtx; /* 36 bytes */

static inline uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }
static inline uint64_t ld64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* vu1_native.c's tag_ok: a strip's tag as the programs send it (PRE, packed ST RGBAQ XYZF2, the type, no fog) */
static int tag_ok(const uint8_t *tag, unsigned type, int eop) {
    uint64_t lo = ld64(tag), hi = ld64(tag + 8), prim = (lo >> 47) & 0x7FF;
    return (!eop || ((lo >> 15) & 1)) && ((lo >> 46) & 1) && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 3 && (hi & 0xFFF) == 0x412 &&
           (prim & 7) == type && !((prim >> 5) & 1) && !((prim >> 8) & 1) && (lo & 0x7FFF) != 0;
}

/* ad_tag_ok: the A+D tag of one register a batch starts with */
static int ad_tag_ok(const uint8_t *tag) {
    uint64_t lo = ld64(tag), hi = ld64(tag + 8);
    return (lo & 0x7FFF) == 1 && ((lo >> 58) & 3) == 0 && ((lo >> 60) & 15) == 1 && (hi & 15) == 14;
}

static inline const uint8_t *fig_at(const void *chain) {
    const uint8_t *c = (const uint8_t *)chain;
    return c != NULL && rd32(c + 4) == FIG_MAGIC ? c : NULL;
}

/* One batch (fig_batch's checks, program 0's: the strictest): the header's five quadwords (A+D tag, TEX0 of each
   layer, the two strips' tags), three quadwords a corner (position with the weight, normal, s t q). out: where its
   FigBatch goes (write 0: only checked, its room too: nothing written past what is read). 0 = not read. */
static int fig_conv_batch(uint8_t **out, const uint8_t *hdr, const uint8_t *vtx, uint32_t nv, int write) {
    FigBatch b;
    uint32_t n, i, c;

    if (!ad_tag_ok(hdr) || !tag_ok(hdr + 48, 4, 0) || !tag_ok(hdr + 64, 4, 1)) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.prim[0] = (uint32_t)(ld64(hdr + 48) >> 47) & 0x7FF;
    b.prim[1] = (uint32_t)(ld64(hdr + 64) >> 47) & 0x7FF;
    n = rd32(hdr + 48) & 0x7FFF;
    b.reg[0] = (uint8_t)rd32(hdr + 24);
    b.reg[1] = (uint8_t)rd32(hdr + 40);
    if (n > FIG_MAXV || n * 3 > nv || (rd32(hdr + 64) & 0x7FFF) != n || b.reg[0] != 6 + ((b.prim[0] >> 9) & 1) ||
        b.reg[1] != 6 + ((b.prim[1] >> 9) & 1)) {
        return 0;
    }
    for (c = 0; c < 2; c++) {
        b.tex0[c][0] = rd32(hdr + 16 + c * 16);
        b.tex0[c][1] = rd32(hdr + 20 + c * 16);
    }
    b.n = (uint16_t)n;
    b.fade = (rd32(vtx + 28) & 0xFFFF) != 0;
    if (*out + sizeof(b) > hdr + 80) {
        return 0;
    }
    if (write) { memcpy(*out, &b, sizeof(b)); }
    *out += sizeof(b);
    for (i = 0; i < n; i++) {
        const float *f = (const float *)(vtx + i * 48);
        float q = f[10] != 0.0f ? f[10] : 1.0f;
        FigVtx v;
        if (*out + sizeof(v) > (const uint8_t *)(f + 12)) {
            return 0;
        }
        v.x = f[0];
        v.y = f[1];
        v.z = f[2];
        v.w = f[3];
        v.nx = f[4];
        v.ny = f[5];
        v.nz = f[6];
        v.u = f[8] / q;
        v.v = f[9] / q;
        if (write) { memcpy(*out, &v, sizeof(v)); }
        *out += sizeof(v);
    }
    return 1;
}

/* The chain as a file (vu1_native.c walk_vif): one DMA ret tag, its VIF codes; UNPACK V4-32 of the header at 0 and
   the corners behind it (or both in one), MSCNT runs it. */
static int fig_walk(uint8_t *chain, int write, uint32_t *batches, uint32_t *corners) {
    uint32_t t0 = rd32(chain), nh = 0, nv = 0;
    const uint8_t *w = chain + 8, *end = chain + 16 + (t0 & 0xFFFF) * 16, *hdr = NULL, *vtx = NULL;
    uint8_t *out = chain + FIG_DATA;

    if (((t0 >> 28) & 7) != 6 || rd32(chain + 4) != 0) {
        return 0;
    }
    *batches = *corners = 0;
    while (w < end) {
        uint32_t code = rd32(w), cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
        w += 4;
        if (cmd == 0x00 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13) {
            continue;
        }
        if (cmd == 0x01) {
            if (imm != 0x0101) { return 0; }
            continue;
        }
        if (cmd == 0x6C) {
            uint32_t num = (code >> 16) & 0xFF;
            num = num ? num : 256;
            if (!(imm & 0x8000) || w + num * 16 > end) {
                return 0;
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
                return 0;
            }
            w += num * 16;
            continue;
        }
        if (cmd != 0x17 || hdr == NULL) {
            return 0;
        }
        if (vtx == NULL && nh > 5) { /* the header and the corners in one UNPACK */
            vtx = hdr + 80;
            nv = nh - 5;
            nh = 5;
        }
        if (nh != 5 || vtx == NULL || !fig_conv_batch(&out, hdr, vtx, nv, write)) {
            return 0;
        }
        (*batches)++;
        *corners += rd32(hdr + 48) & 0x7FFF;
        hdr = vtx = NULL;
        nh = nv = 0;
    }
    return *batches != 0;
}

static unsigned sRead, sNotRead; /* TEMP */

/* The chain read in place: checked whole first, then written. NULL = not read (as it was). */
static const uint8_t *fig_read(void *chain) {
    uint8_t *c = (uint8_t *)chain;
    uint32_t nb, nc;
    if (c == NULL || !fig_walk(c, 0, &nb, &nc)) {
        sNotRead++;
        return NULL;
    }
    fig_walk(c, 1, &nb, &nc);
    *(uint32_t *)(c + 8) = nb;
    *(uint32_t *)(c + 12) = nc;
    *(uint32_t *)(c + 4) = FIG_MAGIC;
    sRead++;
    return c;
}

/* ---- a model's parts, one draw ------------------------------------------------------------------------------ */

#define FIG_NOTES 24
#define FIG_PARTS 64 /* parts of one draw (its record 64 * 200 bytes and a little: the queue's blocks are 32 KB) */

typedef struct FigPart {
    const uint8_t *chain;
    uint32_t pad;
    float a[16], b[16]; /* the bones with their pivots in: A (p - ofsA) = a p */
    float l0[4], l1[4]; /* program 0: (light A).x, (light B).x a row each; program 1: the light's rows x, y */
    float col[2][4];    /* each layer's colour (program 2: one) */
} FigPart; /* 200 bytes */

typedef struct FigRec {
    RndSnap st;
    uint32_t prog, alt, notes, parts;
    float s[16], c[16];  /* screen, clip (one for every part ObjDraw_DrawParts draws at once) */
    uint32_t fadeTex[2]; /* program 1: the second layer's TEX0 (constant 26) */
    RndTexNote note[FIG_NOTES];
    FigPart part[];
} FigRec;

static unsigned sN[11]; /* TEMP: parts drawn by program 0, 1, 2; the old way; strips by records; batches; headers;
                          program 0's batches with the ramp folded in, with it a layer of its own; draws by four transforms */
static uint64_t sNs;
/* TEMP: fig_draw's steps: the batch table and keys, Rnd_Direct, chunk_make, the corners written and sent, the
   records' strips; the queueing (FigNative_Draw and End, on the game's side); calls of chunk_make, corners made,
   vertices sent */
static uint64_t sTm[6];
static unsigned sCm[4]; /* (3: corners one bone's) */
static uint64_t sSt[5]; /* TEMP: inside skinning, per batch: bone A, bone B and the clip test, the screen and the rest,
                           the timer itself (two calls back to back); batches timed */

/* m with its pivot taken first: m (p - o) = m' p (w 1) */
static void shifted(float *o, const float *m, const float *ofs) {
    int r;
    memcpy(o, m, 64);
    for (r = 0; r < 4; r++) { o[12 + r] = m[12 + r] - (m[r] * ofs[0] + m[4 + r] * ofs[1] + m[8 + r] * ofs[2]); }
}

/* the TEX0 a layer of a batch is drawn with, its PRIM as the GS draws by it (the state of then put back) */
static uint64_t layer_tex0(const FigRec *r, const FigBatch *b, int layer, uint32_t prim) {
    uint32_t ctx = (prim >> 9) & 1;
    if (r->prog == 1 && layer == 1) {
        return (uint64_t)r->fadeTex[1] << 32 | r->fadeTex[0];
    }
    if (r->prog == 2) { /* (2's one strip is layer 1's tag with layer 0's TEX0, or the one bound) */
        return b->reg[0] == 6 + ctx ? (uint64_t)b->tex0[0][1] << 32 | b->tex0[0][0] : Rnd_Tex0((int)ctx);
    }
    return (uint64_t)b->tex0[layer][1] << 32 | b->tex0[layer][0];
}

static inline int layers_of(const FigRec *r) { return r->prog == 2 ? 1 : 2; }
static inline int layer_prim(const FigRec *r, int layer) { return r->prog == 2 ? 1 : layer; }

/* ---- the shading ramp in the corners' colour ------------------------------------------------------------------
   Program 0's second layer is the ramp (the light's texture 0 at 0x3D40 through the model's palette at its CBP) at
   u = the light on the normal, subtracted from what the first layer drew (ALPHA Cd - Cs by 1, which gs_pvr.c draws as
   the destination times one minus the source). Here the same a corner at a time: the first layer's colour times one
   minus the ramp's at the corner's u, and no second layer. A batch whose ramp is not read this way, or whose state
   is another, keeps its second layer. */
#define FIG_RAMPS 16
#define FIG_RAMPW 64
typedef struct FigRamp {
    uint64_t tex0;
    uint32_t n;              /* texels across (sampled down to FIG_RAMPW); 0 = not read */
    uint32_t decal;          /* TFX decal: its colour as it is, not times the layer's */
    float fn;                /* n */
    uint32_t tab[FIG_RAMPW]; /* the folded colours (tab_of), for tabArgb and tabC1; tabOk 0: none yet */
    uint32_t tabArgb, tabC1, tabOk;
    uint32_t col[FIG_RAMPW]; /* as GS memory holds them: r in the low byte */
} FigRamp;
static FigRamp sRamp[FIG_RAMPS];
static uint32_t sRamps; /* (read anew each draw: the palettes change) */
static unsigned sRampWhy[6]; /* TEMP: not read: no note, no entry, the format, no palette, the packet, too wide */

extern const void *GsPvr_TexEntry(uint32_t tex);            /* gs/gs_pvr.c */
extern const uint32_t *GsPvr_PalCol(uint32_t pal, uint32_t n);

/* GS memory's layouts (port/tools/dc_tex.c vram_rw): where pixel (x, y) of a 32-bit buffer of width bw (in 64s) lies,
   in words from its first block; where an 8-bit one's lies, in bytes */
static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol8First[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                                          {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};
static inline uint32_t at32(uint32_t bw, uint32_t x, uint32_t y) {
    return (((y & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + kBlock32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kCol32[y & 7][x & 7];
}
static inline uint32_t at8(uint32_t bw, uint32_t x, uint32_t y) {
    uint32_t col = (y & 15) >> 2;
    return ((((y >> 1) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) +
           kCol8First[y & 3][(col & 1) ? ((x & 15) ^ 4) : (x & 15)] + col * 64;
}

/* the 8-bit indices of row 0, x 0..n-1 (of tw) of an 8-bit texture uploaded as 32 bits (rw x rh, width dbw): the upload
   laid out as GS memory would hold it, read as TEX0 does (width tbw). 0 = it lies beyond two pages. Kept: the light's
   texture is uploaded once. */
#define FIG_T8WORDS 4096 /* (two pages) */
static int t8_from_32(uint8_t *idx, uint32_t n, uint32_t tw, const uint8_t *img, uint32_t rw, uint32_t rh, uint32_t dbw, uint32_t tbw) {
    static uint32_t mem[FIG_T8WORDS];
    uint32_t x, y;
    for (y = 0; y < rh; y++) {
        for (x = 0; x < rw; x++) {
            uint32_t a = at32(dbw ? dbw : 1, x, y);
            if (a >= FIG_T8WORDS) { return 0; }
            mem[a] = rd32(img + (y * rw + x) * 4);
        }
    }
    for (x = 0; x < n; x++) {
        uint32_t a = at8(tbw ? tbw : 1, x * tw / n, 0);
        if (a >= FIG_T8WORDS * 4) { return 0; }
        idx[x] = (uint8_t)(mem[a >> 2] >> ((a & 3) * 8));
    }
    return 1;
}

/* the ramp's first row from its entry's upload packet (TRXREG, then IMAGE), in the format TEX0 reads it */
static void ramp_read(FigRamp *o, const RndTexNote *note) {
    uint64_t t0 = o->tex0;
    uint32_t psm = (uint32_t)(t0 >> 20) & 63, tw = 1u << ((uint32_t)(t0 >> 26) & 15), rw = 0, rh = 0, size, at = 0, x, dpsm;
    static const uint8_t *kImg;   /* (the 8-bit view of a 32-bit upload, kept: its packet, TEX0's shape) */
    static uint64_t kShape;
    static uint8_t kIdx[FIG_RAMPW];
    const uint8_t *e, *p, *img = NULL;
    const uint32_t *pal = NULL;

    o->n = 0;
    if (note == NULL) { sRampWhy[0]++; return; }
    if ((e = (const uint8_t *)GsPvr_TexEntry(note->tex & ~RND_TEX_RGB)) == NULL) { sRampWhy[1]++; return; }
    dpsm = (rd32(e + 0x18) >> 8) & 63;
    if ((psm != 0x13 && psm != 0x14 && psm != 0) || (dpsm != psm && !(psm == 0x13 && dpsm == 0)) || ((t0 >> 56) & 31) != 0) {
        sRampWhy[2]++;
        return;
    }
    if (psm != 0 && (pal = GsPvr_PalCol(note->pal, psm == 0x13 ? 256 : 16)) == NULL) { sRampWhy[3]++; return; }
    p = (const uint8_t *)Port_Ptr(rd32(e + 0x38));
    size = rd32(e + 8);
    if (size >= 16 && (p[15] & 0x7F) == 0x50) { at = 16; } /* (VIF DIRECT) */
    while (at + 16 <= size && img == NULL) {
        uint64_t tag = ld64(p + at);
        uint32_t nloop = (uint32_t)tag & 0x7FFF, flg = (uint32_t)(tag >> 58) & 3, i;
        at += 16;
        if (flg == 0 && ((tag >> 60) & 15) == 1 && (p[at - 8] & 15) == 0xE) { /* A+D: TRXREG */
            for (i = 0; i < nloop && at + 16 <= size; i++, at += 16) {
                if (p[at + 8] == 0x52) { rw = rd32(p + at) & 0xFFF; rh = rd32(p + at + 4) & 0xFFF; }
            }
        } else if (flg == 2) { /* IMAGE */
            img = p + at;
        } else {
            break;
        }
    }
    o->n = tw < FIG_RAMPW ? tw : FIG_RAMPW;
    o->fn = (float)(int32_t)o->n;
    o->tabOk = 0;
    if (img != NULL && dpsm != psm) { /* (8-bit read from a 32-bit upload: the light's) */
        if (kImg != img || kShape != (t0 & 0xFFFFC000ull)) {
            kImg = NULL;
            if (!t8_from_32(kIdx, o->n, tw, img, rw, rh, rd32(e + 0x18) & 63, (uint32_t)(t0 >> 14) & 63)) {
                o->n = 0;
                sRampWhy[4]++;
                return;
            }
            kImg = img;
            kShape = t0 & 0xFFFFC000ull;
        }
        for (x = 0; x < o->n; x++) { o->col[x] = pal[kIdx[x]]; }
        o->decal = ((t0 >> 35) & 3) == 1;
        return;
    }
    if (img == NULL || rw < tw) { o->n = 0; sRampWhy[4]++; return; }
    for (x = 0; x < o->n; x++) { /* (texel x * tw / n, nearest) */
        uint32_t k = x * tw / o->n;
        o->col[x] = psm == 0 ? rd32(img + k * 4) : psm == 0x13 ? pal[img[k]] : pal[(img[k >> 1] >> ((k & 1) * 4)) & 15];
    }
    o->decal = ((t0 >> 35) & 3) == 1;
}

/* the ramp of TEX0 t (one of the draw's notes), read; -1 = none */
static int ramp_of(const FigRec *r, uint64_t t) {
    uint32_t i;
    const RndTexNote *note = NULL;
    for (i = 0; i < sRamps; i++) {
        if (sRamp[i].tex0 == t) { return sRamp[i].n ? (int)i : -1; }
    }
    if (sRamps == FIG_RAMPS) {
        return -1;
    }
    for (i = 0; i < r->notes; i++) {
        if (r->note[i].tex0 == t) { note = &r->note[i]; }
    }
    sRamp[sRamps].tex0 = t;
    ramp_read(&sRamp[sRamps], note);
    {
        static int told; /* TEMP */
        if (!told) {
            told = 1;
            fprintf(stderr, "fig_native: the shading ramp, TEX0 %08x%08x: %u texels (%08x %08x %08x ...), not read %u %u %u %u %u %u\n", 
                    (unsigned)(t >> 32), (unsigned)t, (unsigned)sRamp[sRamps].n, (unsigned)sRamp[sRamps].col[0],
                    (unsigned)sRamp[sRamps].col[sRamp[sRamps].n / 2], (unsigned)sRamp[sRamps].col[sRamp[sRamps].n ? sRamp[sRamps].n - 1 : 0],
                    sRampWhy[0], sRampWhy[1], sRampWhy[2], sRampWhy[3], sRampWhy[4], sRampWhy[5]);
        }
    }
    sRamps++;
    return sRamp[sRamps - 1].n ? (int)sRamps - 1 : -1;
}

/* the batch's ramp to fold into its first layer's colour, or -1 (its second layer drawn) */
static int fold_of(const FigRec *r, const FigBatch *b) {
    uint32_t p0, p1, c1;
    uint64_t t0, t1;
    const RndState *s;
    if (r->prog != 0) {
        return -1;
    }
    p0 = Rnd_Prim(b->prim[0]);
    p1 = Rnd_Prim(b->prim[1]);
    c1 = (p1 >> 9) & 1;
    s = &gRndCtx[c1];
    t0 = layer_tex0(r, b, 0, p0);
    t1 = layer_tex0(r, b, 1, p1);
    { /* (the ramp subtracted by 1, modulated or decal; the first layer's colour modulating) */
        int why = !((p1 >> 6) & 1) ? 1 : !((p1 >> 4) & 1) ? 2 : gRndCtxBad[c1] != 0 ? 3 : s->blend != RND_BLEND_SUB ? 4 :
                  (s->flags & (RND_ONE | RND_FIX | RND_DEST_ALPHA)) != RND_ONE ? 5 : ((t1 >> 35) & 3) > 1 ? 6 :
                  (((p0 >> 4) & 1) && ((t0 >> 35) & 3) != 0) ? 7 : 0;
        static int told; /* TEMP */
        if (why != 0) {
            if (told < 4) {
                told++;
                fprintf(stderr, "fig_native: ramp not folded (%d): prim %03x / %03x, ctx %u bad %x, blend %u flags %04x, "
                                "TEX0 %08x%08x / %08x%08x\n", why, (unsigned)p0, (unsigned)p1, (unsigned)c1, (unsigned)gRndCtxBad[c1],
                        (unsigned)s->blend, (unsigned)s->flags, (unsigned)(t0 >> 32), (unsigned)t0, (unsigned)(t1 >> 32), (unsigned)t1);
            }
            return -1;
        }
    }
    return ramp_of(r, t1);
}

/* ---- made and sent ---------------------------------------------------------------------------------------- */

/* every batch of the draw, with its part, its key in the layer being drawn, the draw's order by key */
#define FIG_BATCHES 4096
#define FIG_KEYS 128
static const FigBatch *sBt[FIG_BATCHES];
static uint8_t sPart[FIG_BATCHES], sDone[FIG_BATCHES], sKid[FIG_BATCHES];
static uint32_t sPrim[FIG_BATCHES];
static uint64_t sTex[FIG_BATCHES];
static uint16_t sOrder[FIG_BATCHES];

/* The screen matrix as the clip one and the viewport (vu1_native.c fig_prepare): s.x = c.x v0 + c.w v4,
   s.y = c.y v1 + c.w v5, s.z = c.w v2 + v3, s.w = c.w. Then a corner takes two transforms (bones A and B in the
   clip volume), not four. 0 = not so (the four). */
static int screen_rel(const float *S, const float *C, float *v) {
    float best = 0.0f, scale = 0.0f;
    int j, a = 0, b = 1, r, ok = 1;
    for (j = 0; j < 16; j++) {
        float m = S[j] < 0.0f ? -S[j] : S[j];
        scale = m > scale ? m : scale;
    }
    for (r = 0; r < 2; r++) { /* rows x and y: two unknowns, from the two columns that tell them apart best */
        int i, k;
        best = 0.0f;
        for (i = 0; i < 4; i++) {
            for (k = i + 1; k < 4; k++) {
                float det = C[i * 4 + r] * C[k * 4 + 3] - C[k * 4 + r] * C[i * 4 + 3];
                det = det < 0.0f ? -det : det;
                if (det > best) { best = det; a = i; b = k; }
            }
        }
        if (best == 0.0f) {
            return 0;
        }
        {
            float det = C[a * 4 + r] * C[b * 4 + 3] - C[b * 4 + r] * C[a * 4 + 3];
            v[r] = (S[a * 4 + r] * C[b * 4 + 3] - S[b * 4 + r] * C[a * 4 + 3]) / det;
            v[4 + r] = (C[a * 4 + r] * S[b * 4 + r] - C[b * 4 + r] * S[a * 4 + r]) / det;
        }
    }
    best = 0.0f;
    for (j = 0; j < 3; j++) { /* row z: v2 from a column with w in it, v3 from the fourth */
        float m = C[j * 4 + 3] < 0.0f ? -C[j * 4 + 3] : C[j * 4 + 3];
        if (m > best) { best = m; a = j; }
    }
    if (best == 0.0f) {
        return 0;
    }
    v[2] = S[a * 4 + 2] / C[a * 4 + 3];
    v[3] = S[14] - v[2] * C[15];
    for (j = 0; j < 4 && ok; j++) { /* every entry so, or not at all */
        float e[4], tol = scale * 1e-4f + 1e-6f;
        e[0] = C[j * 4] * v[0] + C[j * 4 + 3] * v[4];
        e[1] = C[j * 4 + 1] * v[1] + C[j * 4 + 3] * v[5];
        e[2] = C[j * 4 + 3] * v[2] + (j == 3 ? v[3] : 0.0f);
        e[3] = C[j * 4 + 3];
        for (r = 0; r < 4; r++) {
            float d = e[r] - S[j * 4 + r];
            ok &= d <= tol && d >= -tol;
        }
    }
    return ok;
}

/* o = l r (4 x 4, columns) */
static void mat_mul(float *o, const float *l, const float *r) {
    int i, j;
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            o[j * 4 + i] = l[i] * r[j * 4] + l[4 + i] * r[j * 4 + 1] + l[8 + i] * r[j * 4 + 2] + l[12 + i] * r[j * 4 + 3];
        }
    }
}

/* What the corners are made into: x = X q kx + bx (X: the clip volume's x with the viewport, or the screen's),
   z = za + q (zb + Z zc) held to z0..z1, u v times us vs; the PVR's (a header open) or the GS's units (by records). */
typedef struct FigMap {
    float kx, bx, ky, by, za, zb, zc, z0, z1, us, vs;
    uint32_t argb;      /* the layer's colour (a batch not folded) */
    uint32_t c0[4], c1[3]; /* folded: the first layer's colour as the PVR's 0..255, the ramp's (the second's) */
} FigMap;

/* the corners of some of a part's batches, made: PVR vertices (flags left out), q for the records' way */
#define FIG_CHUNK 1536
typedef struct FigOut {
    uint32_t pad;
    float x, y, z, u, v;
    uint32_t argb;
    float q;
} FigOut; /* (pvr_vertex_t's layout, q in oargb's place) */

static FigOut sOut[FIG_CHUNK] __attribute__((aligned(32)));
static uint8_t sAdc[FIG_CHUNK];
static float sMat[FIG_PARTS][4][16]; /* each part's clip A, clip B; screen A, screen B (not screen_rel) */
static int sRel;
static float sV[6];
static uint8_t sFold[FIG_BATCHES]; /* the batch's ramp (sRamp), 0xFF none */

static uint32_t unit(int32_t c) {
    c &= 0xFF;
    return c >= 0x80 ? 255u : (uint32_t)c * 2u;
}

static inline int32_t ftoi(float f) {
    return f >= 2147483648.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
}

/* ramp ri's folded colours for map m's, one a texel: c0 (1 - ramp c1), as the PVR draws the two layers; kept with
   the ramp, made again only for other colours (the parts' are mostly the same) */
static __attribute__((noinline)) const uint32_t *tab_of(const FigMap *m, int ri) {
    FigRamp *rp = &sRamp[ri];
    uint32_t c1 = m->c1[0] | m->c1[1] << 8 | m->c1[2] << 16;
    if (rp->tabOk && rp->tabArgb == m->argb && (rp->decal || rp->tabC1 == c1)) {
        return rp->tab;
    }
    sN[10]++; /* TEMP */
    uint32_t x, ch;
    for (x = 0; x < rp->n; x++) {
        uint32_t o = m->c0[3] << 24;
        for (ch = 0; ch < 3; ch++) {
            uint32_t cs = rp->decal ? (rp->col[x] >> (ch * 8)) & 0xFF
                                    : (((rp->col[x] >> (ch * 8)) & 0xFF) * m->c1[ch] * 257u + 257u) >> 16; /* (/ 255: no divide) */
            o |= ((m->c0[ch] * (255u - (cs > 255u ? 255u : cs)) * 257u + 257u) >> 16) << (16 - ch * 8);
        }
        rp->tab[x] = o;
    }
    rp->tabOk = 1;
    rp->tabArgb = m->argb;
    rp->tabC1 = c1;
    return rp->tab;
}

/* a batch's corners from the clip volume (or the screen): to the screen, their texture coordinates and colours.
   Not inlined, and the map's numbers held in locals: in fig_draw it ran out of registers and read them from the
   stack a corner at a time. The position first (corners_pos), then a loop for each kind of colour and texture
   coordinates. */
static __attribute__((noinline)) void corners_pos(const float (*t)[4], FigOut *out, uint32_t n, const FigMap *m) {
    const float kx = m->kx, bx = m->bx, ky = m->ky, by = m->by, za = m->za, zb = m->zb, zc = m->zc, z0 = m->z0,
                z1 = m->z1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        float w = t[i][3], q = w != 0.0f ? shz_invf(w) : 0.0f, z = za + q * (zb + t[i][2] * zc);
        FigOut *o = &out[i];
        o->x = t[i][0] * q * kx + bx;
        o->y = t[i][1] * q * ky + by;
        o->z = z < z0 ? z0 : z > z1 ? z1 : z;
        o->q = q;
    }
}

static __attribute__((noinline)) void corners_put(const FigRec *r, const FigPart *pt, uint32_t bk, const FigBatch *b,
                                                  float (*t)[4], FigOut *out, int layer, const FigMap *m) {
    const FigVtx *v = (const FigVtx *)(b + 1);
    const uint32_t n = b->n;
    const float us = m->us, vs = m->vs;
    const float a0 = pt->l0[0], a1 = pt->l0[1], a2 = pt->l0[2], a3 = pt->l0[3];
    const float b0 = pt->l1[0], b1 = pt->l1[1], b2 = pt->l1[2], b3 = pt->l1[3];
    int ri = layer == 0 && sFold[bk] != 0xFF ? (int)sFold[bk] : -1;
    uint32_t i;

    corners_pos((const float (*)[4])t, out, n, m);
    if (ri >= 0) { /* the first layer, the ramp's texel at the light's u (nearest) folded in */
        const uint32_t *tab = tab_of(m, ri);
        const float h = 0.5f * sRamp[ri].fn;
        const int32_t top = (int32_t)sRamp[ri].n - 1;
        for (i = 0; i < n; i++) {
            const FigVtx *c = &v[i];
            float la = a0 * c->nx + a1 * c->ny + a2 * c->nz + a3, lb = b0 * c->nx + b1 * c->ny + b2 * c->nz + b3;
            int32_t x = (int32_t)((lb + (la - lb) * c->w) * h + h);
            out[i].u = c->u * us;
            out[i].v = c->v * vs;
            out[i].argb = tab[x < 0 ? 0 : x > top ? top : x];
        }
    } else if (layer == 0) {
        const uint32_t argb = m->argb;
        for (i = 0; i < n; i++) {
            out[i].u = v[i].u * us;
            out[i].v = v[i].v * vs;
            out[i].argb = argb;
        }
    } else if (r->prog == 0) { /* u = 0.5 + 0.5 (light on the normal, blended by the weight).x */
        const uint32_t argb = m->argb;
        const float hu = 0.5f * us;
        for (i = 0; i < n; i++) {
            const FigVtx *c = &v[i];
            float la = a0 * c->nx + a1 * c->ny + a2 * c->nz + a3, lb = b0 * c->nx + b1 * c->ny + b2 * c->nz + b3;
            out[i].u = (lb + (la - lb) * c->w) * hu + hu;
            out[i].v = 0.0f;
            out[i].argb = argb;
        }
    } else { /* program 1: (u, v) = 0.5 (1 + (light on the normal).xy) */
        const uint32_t argb = m->argb;
        const float hu = 0.5f * us, hv = 0.5f * vs;
        for (i = 0; i < n; i++) {
            const FigVtx *c = &v[i];
            out[i].u = (a0 * c->nx + a1 * c->ny + a2 * c->nz + a3) * hu + hu;
            out[i].v = (b0 * c->nx + b1 * c->ny + b2 * c->nz + b3) * hv + hv;
            out[i].argb = argb;
        }
    }
}

/* the batches bt[0..nb) of part pi (from the draw's ki on: their folds) in layer `layer`: skinned in the clip volume
   (bone A, bone B, blended by the weight), the triangles outside it marked (ADC: four screens wide, or behind the near
   plane, as vu1_native.c clip_flags), to the screen; their texture coordinates and colours */
static void chunk_make(const FigRec *r, int pi, const uint16_t *ks, uint32_t nb, int layer, const FigMap *m) {
    const FigPart *pt = &r->part[pi];
    uint32_t i, k, j;

    for (k = 0, j = 0; k < nb; j += sBt[ks[k]]->n, k++) { /* (a batch at a time: its corners stay in the cache) */
        const FigBatch *b = sBt[ks[k]];
        const FigVtx *v = (const FigVtx *)(b + 1);
        static float t[FIG_MAXV][4] __attribute__((aligned(32))); /* (one batch's: in the cache) */
        uint32_t f0 = 0, f1 = 0, f2 = 0;
        uint64_t s0 = Gs_NowNs(), s1, s2; /* TEMP */
        shz_xmtrx_load_unaligned_4x4(sMat[pi][0]);
        for (i = 0; i < b->n; i++) { /* (bone A: not for a corner wholly B's) */
            if (v[i].w != 0.0f) {
                shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
                t[i][0] = p.x;
                t[i][1] = p.y;
                t[i][2] = p.z;
                t[i][3] = p.w;
            }
        }
        s1 = Gs_NowNs(); /* TEMP */
        sSt[0] += s1 - s0;
        shz_xmtrx_load_unaligned_4x4(sMat[pi][1]);
        for (i = 0; i < b->n; i++) {
            float w = v[i].w, cx, cy, cz, cw, g;
            sCm[3] += w == 0.0f || w == 1.0f; /* TEMP */
            if (w == 1.0f) { /* (wholly A's) */
                cx = t[i][0];
                cy = t[i][1];
                cz = t[i][2];
                cw = t[i][3];
            } else {
                shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
                if (w == 0.0f) {
                    cx = p.x;
                    cy = p.y;
                    cz = p.z;
                    cw = p.w;
                } else {
                    cx = p.x + (t[i][0] - p.x) * w;
                    cy = p.y + (t[i][1] - p.y) * w;
                    cz = p.z + (t[i][2] - p.z) * w;
                    cw = p.w + (t[i][3] - p.w) * w;
                }
            }
            g = cw * 4.0f; /* (no far plane; w <= 0 outside) */
            f0 = f1;
            f1 = f2;
            f2 = cx > g || cx < -g || cy > g || cy < -g || cz < -cw;
            sAdc[j + i] = (uint8_t)(f0 | f1 | f2);
            t[i][0] = cx;
            t[i][1] = cy;
            t[i][2] = cz;
            t[i][3] = cw;
        }
        if (!sRel) { /* (the screen by its own matrices) */
            float sc[FIG_MAXV][4];
            shz_xmtrx_load_unaligned_4x4(sMat[pi][2]);
            for (i = 0; i < b->n; i++) {
                shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
                sc[i][0] = p.x;
                sc[i][1] = p.y;
                sc[i][2] = p.z;
                sc[i][3] = p.w;
            }
            shz_xmtrx_load_unaligned_4x4(sMat[pi][3]);
            for (i = 0; i < b->n; i++) {
                shz_vec4_t p = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
                float w = v[i].w;
                t[i][0] = p.x + (sc[i][0] - p.x) * w;
                t[i][1] = p.y + (sc[i][1] - p.y) * w;
                t[i][2] = p.z + (sc[i][2] - p.z) * w;
                t[i][3] = p.w + (sc[i][3] - p.w) * w;
            }
        }
        s2 = Gs_NowNs(); /* TEMP */
        sSt[1] += s2 - s1;
        corners_put(r, pt, ks[k], b, t, &sOut[j], layer, m);
        s1 = Gs_NowNs(); /* TEMP */
        sSt[2] += s1 - s2;
        sSt[3] += Gs_NowNs() - s1;
        sSt[4]++;
    }
}

/* each run of a batch's triangles not left out (adc) as one of the PVR's strips, straight to the TA */
static uint32_t batch_send(const FigOut *o, const uint8_t *adc, uint32_t n) {
    uint32_t k = 2, e, i, sent = 0;
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
            pv->x = o[i].x;
            pv->y = o[i].y;
            pv->z = o[i].z;
            pv->u = o[i].u;
            pv->v = o[i].v;
            pv->argb = o[i].argb;
            pv->oargb = 0;
            pvr_dr_commit(pv);
        }
        sent += e - k + 2;
        k = e;
    }
    return sent;
}

/* A strip the PVR path does not take: as the programs' records (Rnd_DrawStrip; o in the GS's units) */
static void strip_old(const FigBatch *b, const FigOut *o, const uint8_t *adc, uint32_t prim, uint64_t tex0) {
    static RndVtx rv[FIG_MAXV];
    uint32_t i, ch, ctx = (prim >> 9) & 1;
    RndPoly h;
    for (i = 0; i < b->n; i++) {
        uint32_t rgba = 0;
        for (ch = 0; ch < 4; ch++) { /* (unit's inverse) */
            uint32_t c = (o[i].argb >> (ch == 3 ? 24 : 16 - ch * 8)) & 0xFF;
            rgba |= (c >= 255u ? 0x80u : c >> 1) << (ch * 8);
        }
        Rnd_VtxUnwrapped(&rv[i], (int)ctx, ftoi(o[i].x * 16.0f), ftoi(o[i].y * 16.0f), ((uint32_t)ftoi(o[i].z * 16.0f) >> 4) & 0xFFFFFF,
                         o[i].u * o[i].q, o[i].v * o[i].q, o[i].q, rgba);
    }
    memset(&h, 0, sizeof(h));
    h.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
    h.st.flags = RND_VU;
    Rnd_DrawStrip(&h, rv, adc, b->n, (int)ctx, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1);
    sN[4]++; /* TEMP */
}

/* the map of part pi's corners in layer `layer` under header d (open 1) or by records (0) */
static void map_make(FigMap *m, const FigRec *r, int pi, int layer, int open, const RndDirect *d) {
    const float *cf = r->part[pi].col[r->prog == 2 ? 0 : layer];
    const float *c1 = r->part[pi].col[1];
    int32_t col[4];
    uint32_t i;
    float vx = sRel ? sV[0] : 1.0f, vy = sRel ? sV[1] : 1.0f, ox = sRel ? sV[4] : 0.0f, oy = sRel ? sV[5] : 0.0f;
    float za = sRel ? sV[2] : 0.0f, zb = sRel ? sV[3] : 0.0f, zc = sRel ? 0.0f : 1.0f;

    for (i = 0; i < 4; i++) { col[i] = ftoi(cf[i]); }
    if (open == 1) {
        m->kx = vx * d->sx;
        m->bx = (ox - d->xo) * d->sx;
        m->ky = vy * d->sy;
        m->by = (oy - d->yo) * d->sy;
        m->za = za * d->zk + d->zo;
        m->zb = zb * d->zk;
        m->zc = zc * d->zk;
        m->z0 = d->z0;
        m->z1 = d->z1;
        m->us = d->us;
        m->vs = d->vs;
        m->c0[3] = d->fixa >= 0 ? (uint32_t)d->fixa : unit(col[3]);
    } else {
        m->kx = vx;
        m->bx = ox;
        m->ky = vy;
        m->by = oy;
        m->za = za;
        m->zb = zb;
        m->zc = zc;
        m->z0 = -1e30f;
        m->z1 = 1e30f;
        m->us = m->vs = 1.0f;
        m->c0[3] = unit(col[3]);
    }
    for (i = 0; i < 3; i++) {
        m->c0[i] = unit(col[i]);
        m->c1[i] = unit(ftoi(c1[i]));
    }
    m->argb = m->c0[3] << 24 | m->c0[0] << 16 | m->c0[1] << 8 | m->c0[2];
}

/* the batches ks[0..nb) of one key, in part order: made a part (and a chunk) at a time and sent under header d (open
   1) or by records (0) */
static void key_send(const FigRec *r, const uint16_t *ks, uint32_t nb, int layer, uint32_t prim, uint64_t tex0, int open,
                     const RndDirect *d) {
    static const uint8_t none[FIG_MAXV];
    uint32_t s = 0;
    while (s < nb) {
        int pi = sPart[ks[s]];
        uint32_t e = s, total = 0, j, x;
        uint64_t t1, t2;
        FigMap m;
        while (e < nb && sPart[ks[e]] == pi && total + sBt[ks[e]]->n <= FIG_CHUNK) {
            total += sBt[ks[e++]]->n;
        }
        t1 = Gs_NowNs();
        map_make(&m, r, pi, layer, open, d);
        chunk_make(r, pi, ks + s, e - s, layer, &m);
        t2 = Gs_NowNs();
        sTm[2] += t2 - t1;
        sCm[0]++;
        sCm[1] += total;
        for (x = s, j = 0; x < e; x++) {
            const FigBatch *b = sBt[ks[x]];
            const uint8_t *adc = r->prog == 2 && r->alt ? none : &sAdc[j]; /* (2b: every triangle) */
            if (open == 1) {
                uint32_t n = batch_send(&sOut[j], adc, b->n);
                gStatVtx += n;
                sCm[2] += n;
            } else {
                strip_old(b, &sOut[j], adc, prim, tex0);
            }
            j += b->n;
        }
        sTm[open == 1 ? 3 : 4] += Gs_NowNs() - t2;
        s = e;
    }
}


static void fig_draw(const void *blob) {
    static uint32_t kPrim[FIG_KEYS], kTris[FIG_KEYS], kVerts[FIG_KEYS], kAt[FIG_KEYS + 1], kPut[FIG_KEYS];
    static uint64_t kTex[FIG_KEYS];
    const FigRec *r = (const FigRec *)blob;
    uint32_t n = 0, pi, i, k;
    int layer;
    RndSnap now;
    uint64_t t0 = Gs_NowNs(), tAll = t0;

    Rnd_SnapTake(&now);
    Rnd_SnapPut(&r->st);
    Port_TexNotes(r->notes ? r->note : NULL, r->notes);
    sRamps = 0;
    sRel = screen_rel(r->s, r->c, sV);
    sN[9] += !sRel; /* TEMP */
    for (pi = 0; pi < r->parts; pi++) { /* the batches, part by part; each part's matrices */
        const uint8_t *p = r->part[pi].chain + FIG_DATA;
        uint32_t nb = rd32(r->part[pi].chain + 8);
        mat_mul(sMat[pi][0], r->c, r->part[pi].a);
        mat_mul(sMat[pi][1], r->c, r->part[pi].b);
        if (!sRel) {
            mat_mul(sMat[pi][2], r->s, r->part[pi].a);
            mat_mul(sMat[pi][3], r->s, r->part[pi].b);
        }
        for (i = 0; i < nb && n < FIG_BATCHES; i++) {
            const FigBatch *b = (const FigBatch *)p;
            int f;
            p += sizeof(FigBatch) + b->n * sizeof(FigVtx);
            if (r->prog == 1 && !b->fade) { continue; } /* (a batch without the fade layer: program 1 draws nothing) */
            f = fold_of(r, b);
            sFold[n] = f < 0 ? 0xFF : (uint8_t)f;
            sN[7] += f >= 0;              /* TEMP */
            sN[8] += r->prog == 0 && f < 0; /* TEMP */
            sBt[n] = b;
            sPart[n++] = (uint8_t)pi;
        }
    }
    sN[5] += n; /* TEMP */
    for (layer = 0; layer < layers_of(r); layer++) { /* every first layer before any second */
        uint32_t left = 0;
        for (k = 0; k < n; k++) {
            sDone[k] = layer == 1 && sFold[k] != 0xFF; /* (its ramp in its first layer's colour) */
            if (sDone[k]) { continue; }
            sPrim[k] = Rnd_Prim(sBt[k]->prim[layer_prim(r, layer)]);
            sTex[k] = layer_tex0(r, sBt[k], layer, sPrim[k]);
            left++;
        }
        while (left != 0) { /* the batches sorted by key (FIG_KEYS at a time), each key one header */
            uint32_t nk = 0, id;
            for (k = 0; k < n; k++) {
                sKid[k] = 0xFF;
                if (sDone[k]) { continue; }
                for (id = 0; id < nk && (kPrim[id] != sPrim[k] || kTex[id] != sTex[k]); id++) {
                }
                if (id == nk) {
                    if (nk == FIG_KEYS) { continue; } /* (the next round's) */
                    kPrim[nk] = sPrim[k];
                    kTex[nk] = sTex[k];
                    kTris[nk] = kVerts[nk] = kAt[nk] = 0;
                    nk++;
                }
                sKid[k] = (uint8_t)id;
                kTris[id] += sBt[k]->n - 2u;
                kVerts[id] += sBt[k]->n + 2u; /* (as many as the strips send at the most: a bound) */
                kAt[id]++;
            }
            for (id = 0, i = 0; id < nk; id++) {
                uint32_t c = kAt[id];
                kAt[id] = kPut[id] = i;
                i += c;
            }
            kAt[nk] = i;
            for (k = 0; k < n; k++) {
                if (sKid[k] != 0xFF) {
                    sOrder[kPut[sKid[k]]++] = (uint16_t)k;
                    sDone[k] = 1;
                    left--;
                }
            }
            sTm[0] += Gs_NowNs() - t0;
            for (id = 0; id < nk; id++) {
                uint32_t prim = kPrim[id];
                RndPoly h;
                RndDirect d;
                int open;
                uint64_t t1 = Gs_NowNs();
                memset(&h, 0, sizeof(h));
                h.gsTex0 = (prim >> 4) & 1 ? kTex[id] : 0;
                h.st.flags = RND_VU;
                open = Rnd_Direct(&d, &h, (int)(prim >> 9) & 1, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1, kTris[id], kVerts[id]);
                sTm[1] += Gs_NowNs() - t1;
                sN[6]++; /* TEMP */
                if (open != 2) {
                    key_send(r, sOrder + kAt[id], kAt[id + 1] - kAt[id], layer, prim, kTex[id], open, &d);
                }
            }
            t0 = Gs_NowNs();
        }
    }
    Port_TexNotes(NULL, 0);
    Rnd_SnapPut(&now);
    sNs += Gs_NowNs() - tAll;
}

/* ---- queued -------------------------------------------------------------------------------------------- */

static FigPart sPend[FIG_PARTS];
static uint32_t sPendN, sPendProg, sPendAlt, sPendFade[2];
static float sPendS[16], sPendC[16];

/* TEMP: the fighters' cost, every 60 frames */
static void fig_frame(void) {
    static unsigned from;
    if (gGsFrame - from >= 60) {
        unsigned f = gGsFrame - from ? gGsFrame - from : 1;
        fprintf(stderr, "fig_native per frame: %.1f ms; parts lit %u, fade %u, flat %u, old way %u; batches %u, headers %u, "
                        "strips by records %u; chains read %u, not read %u; ramp folded %u, as a layer %u; draws by four transforms %u; ramp tables made %u\n",
                (double)sNs / 1e6 / f, sN[0] / f, sN[1] / f, sN[2] / f, sN[3] / f, sN[5] / f, sN[6] / f, sN[4] / f, sRead,
                sNotRead, sN[7] / f, sN[8] / f, sN[9] / f, sN[10] / f);
        fprintf(stderr, "fig_native steps per frame (ms): table %.2f, Rnd_Direct %.2f, skinning %.2f (%u calls, %u corners, %u one bone's), "
                        "writing and sending %.2f (%u vertices), by records %.2f; queueing (game side) %.2f\n",
                (double)sTm[0] / 1e6 / f, (double)sTm[1] / 1e6 / f, (double)sTm[2] / 1e6 / f, sCm[0] / f, sCm[1] / f, sCm[3] / f,
                (double)sTm[3] / 1e6 / f, sCm[2] / f, (double)sTm[4] / 1e6 / f, (double)sTm[5] / 1e6 / f);
        fprintf(stderr, "fig_native skinning per frame (ms): bone A %.2f, bone B and clip %.2f, screen and the rest %.2f; "
                        "the timer %.2f (%u batches, %.2f us a call)\n", (double)sSt[0] / 1e6 / f, (double)sSt[1] / 1e6 / f,
                (double)sSt[2] / 1e6 / f, (double)sSt[3] / 1e6 / f, (unsigned)(sSt[4] / f),
                sSt[4] ? (double)sSt[3] / 1e3 / (double)sSt[4] : 0.0);
        memset(sSt, 0, sizeof(sSt));
        memset(sTm, 0, sizeof(sTm));
        memset(sCm, 0, sizeof(sCm));
        memset(sN, 0, sizeof(sN));
        sNs = 0;
        from = gGsFrame;
    }
}

/* The parts staged so far, one draw where the list is now (ObjDraw_DrawParts: at its loop's end, and before anything
   else goes in the list) */
void FigNative_End(void) {
    uint32_t n = sPendN, bytes, pi, i;
    FigRec *r;
    int now;
    static FigRec *own;
    uint64_t t0;

    if (n == 0) {
        return;
    }
    t0 = Gs_NowNs();
    sPendN = 0;
    bytes = (uint32_t)(sizeof(FigRec) + n * sizeof(FigPart));
    r = Gs_Defers() ? (FigRec *)Gs_DeferPush((bytes + 7) & ~7u, fig_draw, Port_Addr(gDmaCur)) : NULL;
    now = r == NULL;
    if (r == NULL) { /* (not deferred: a buffer of its own, made now) */
        if (own == NULL && (own = (FigRec *)malloc(sizeof(FigRec) + FIG_PARTS * sizeof(FigPart))) == NULL) {
            return;
        }
        r = own;
    }
    Rnd_SnapTake(&r->st);
    r->prog = sPendProg;
    r->alt = sPendAlt;
    r->parts = n;
    memcpy(r->s, sPendS, 64);
    memcpy(r->c, sPendC, 64);
    memcpy(r->fadeTex, sPendFade, 8);
    memcpy(r->part, sPend, n * sizeof(FigPart));
    r->notes = 0;
    for (pi = 0; pi < n; pi++) { /* the textures' handles, as they are now */
        const uint8_t *p = r->part[pi].chain + FIG_DATA;
        uint32_t nb = rd32(r->part[pi].chain + 8);
        for (i = 0; i < nb; i++) {
            const FigBatch *b = (const FigBatch *)p;
            int layer;
            p += sizeof(FigBatch) + b->n * sizeof(FigVtx);
            if (r->prog == 1 && !b->fade) { continue; }
            for (layer = 0; layer < layers_of(r); layer++) {
                uint32_t prim = Rnd_Prim(b->prim[layer_prim(r, layer)]), j;
                uint64_t t = layer_tex0(r, b, layer, prim);
                if (!((prim >> 4) & 1) || t == 0) { continue; }
                for (j = 0; j < r->notes && r->note[j].tex0 != t; j++) {
                }
                if (j == r->notes && j < FIG_NOTES) {
                    Port_TexNote(&r->note[r->notes++], t);
                }
            }
        }
    }
    sTm[5] += Gs_NowNs() - t0;
    if (now) { fig_draw(r); }
}

/* 1: the part's chain is drawn here or by vu1_native.c (its constants go in ObjDraw_DrawParts' own block, then
   FigNative_Draw); 0: the VU1 packet's way (what is staged goes in the list first) */
int FigNative_Takes(int prog, void *chain) {
    if (fig_at(chain) != NULL || fig_read(chain) != NULL) {
        return 1;
    }
    FigNative_End();
    return Vu1Native_Takes(prog, chain);
}

/* A part's draw by program prog, with the constants ObjDraw_DrawParts made (ObjDrawPkt from mtxA: bones A, B 0..31,
   pivots 32..39, light 40..55 (program 2: its colour 40..43), screen 56..71, clip 72..87, colours 88..95, program
   1's TEX0 104): staged, a model's parts one draw (FigNative_End) */
void FigNative_Draw(int prog, void *chain, const float *k) {
    const uint8_t *c = fig_at(chain);
    const float *s = k + 56, *cl = k + 72;
    FigPart *pt;
    uint32_t i;
    int alt = 0;

    fig_frame();
    if (c == NULL) { /* (not read: the old way) */
        FigNative_End();
        if (prog == 0) { Vu1Native_Prog0(chain, k); }
        if (prog == 1) { Vu1Native_Prog1(chain, k); }
        if (prog == 2) { Vu1Native_Prog2(chain, k); }
        sN[3]++;
        return;
    }
    if (gPortResim || gsSkipFrame) { /* (a tick without a picture) */
        return;
    }
    if (prog == 2) {
        const float *blk = Vu1Native_Prog2Block(&alt);
        if (blk == NULL) {
            return;
        }
        s = blk + 44;
        cl = blk + 60;
    }
    if (sPendN != 0 && (sPendN == FIG_PARTS || sPendProg != (uint32_t)prog || sPendAlt != (uint32_t)(alt != 0) ||
                        memcmp(sPendS, s, 64) != 0 || memcmp(sPendC, cl, 64) != 0 ||
                        (prog == 1 && memcmp(sPendFade, k + 104, 8) != 0))) {
        FigNative_End();
    }
    if (sPendN == 0) {
        sPendProg = (uint32_t)prog;
        sPendAlt = alt != 0;
        memcpy(sPendS, s, 64);
        memcpy(sPendC, cl, 64);
        if (prog == 1) { memcpy(sPendFade, k + 104, 8); }
    }
    pt = &sPend[sPendN++];
    pt->chain = c;
    shifted(pt->a, k, k + 32);
    shifted(pt->b, k + 16, k + 36);
    if (prog == 2) {
        memcpy(pt->col[0], k + 40, 16);
    } else {
        memcpy(pt->col, k + 88, 32);
    }
    if (prog == 0) { /* (light A).x and (light B).x: the light's x row times each bone's columns (vu1_native.c fig_prepare_light) */
        const float *l = k + 40;
        for (i = 0; i < 3; i++) {
            pt->l0[i] = l[0] * k[i * 4] + l[4] * k[i * 4 + 1] + l[8] * k[i * 4 + 2] + l[12] * k[i * 4 + 3];
            pt->l1[i] = l[0] * k[16 + i * 4] + l[4] * k[16 + i * 4 + 1] + l[8] * k[16 + i * 4 + 2] + l[12] * k[16 + i * 4 + 3];
        }
        pt->l0[3] = pt->l1[3] = l[12];
    } else if (prog == 1) {
        const float *l = k + 40;
        for (i = 0; i < 4; i++) {
            pt->l0[i] = l[i * 4];
            pt->l1[i] = l[i * 4 + 1];
        }
    }
    sN[prog]++; /* TEMP */
}
#endif

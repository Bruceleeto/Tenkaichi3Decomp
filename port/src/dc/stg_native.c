/* The Dreamcast's stage drawn natively (src/battle/stgm_a.c): no VU1, no VIF and no GIF tags at run time.
   - The stage model's groups 0..5 (StgModel_Draw; program 4 on the PS2), the debris (StgModel_DrawDebris; program 7)
     and the animated objects (StgModel_DrawAnims, a node at a time; program 8).
   - Each mesh's chain is read once, as a file, and becomes in its own memory (a corner is 48 bytes there, 32 here)
     PVR-ready vertices: position, s / q and t / q, the colour as ARGB, the PCW with each strip's end, and a
     bounding sphere; program 8's keep each batch's TEX0 and each corner's weight. Groups 0..6 at the stage's first
     draw (StgNative_Begin), program 8's nodes at their first draw.
   - Each frame StgModel_Draw hands over the matrices (StgNative_Begin, in place of Vu1Pkt_LoadProg4) and every visible
     mesh (StgNative_Mesh / Mesh7, in place of Vu1Pkt_CallProg4 / CallProg7); each material's meshes become one draw
     (StgNative_End), each animated node one (StgNative_Mesh8), made where the display list has it (Gs_DeferPush), as
     the other draws are.
   - A draw: one polygon header (Rnd_Direct, the texture StgModel_SetTexture bound; program 8: a batch's TEX0), each
     mesh's sphere against the frustum, then its corners straight to the TA by store queue: one ftrv with the screen
     matrix and the PVR's mapping in it, one reciprocal. A stage mesh that crosses the near plane goes the same way
     with its triangles there cut at it (as DMS's dc_model.c): the TA takes corners off the screen's sides. The
     debris and the animated objects leave such triangles out, as programs 7 and 8 do.
   A mesh whose chain is not read goes the old way (port/src/vu1_native.c); a draw the PVR path does not take
   (Rnd_Direct 0) is not drawn (said once): its chain is no longer there. */
#if defined(__DREAMCAST__) && !defined(BT3_DCSIM) && !defined(BT3_OLDSTAGE)
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
extern void Vu0Screen_StoreMtx(void *m); /* src/sys: what Vu1Pkt_LoadProg4 / CallProg7 / DrawAnims hand the programs */
extern void Vu0Clip_StoreMtx(void *m);

/* ---- the meshes, read once ------------------------------------------------------------------------------- */

/* A mesh once read, in place of its chain:
     chain + 0  the DMA tag's first word, as it was
     chain + 4  the kind's magic (a ret tag's address word, 0 in the game's chains)
     chain + 8  NatMesh
     chain + 48 programs 4, 7: the corners, as the PVR takes them but for the position (PCW: VERTEX, or VERTEX_EOL
                on a strip's last); program 8: batches, each a NatBatch and its corners (w: the weight) */
enum { K4, K7, K8 };
static const uint32_t kMagic[3] = {0x3454414Eu, 0x3754414Eu, 0x3854414Eu}; /* "NAT4", "NAT7", "NAT8" */
#define NAT_DATA 48

typedef struct NatVtx {
    uint32_t flags;
    float x, y, z, u, v;
    uint32_t argb;
    float w; /* program 8: the weight of the node's own matrix */
} NatVtx; /* 32 bytes */

typedef struct NatMesh {
    uint32_t prim;       /* programs 4, 7: its strips' PRIM (one for all of them); program 8: its batches */
    uint32_t verts, tris, pad;
    float cx, cy, cz, r; /* the bounding sphere (not program 8's) */
} NatMesh; /* 32 bytes */

typedef struct NatBatch {
    uint32_t tex0[2]; /* the TEX0 its header writes, with has */
    uint32_t prim, n, has, pad[3];
} NatBatch; /* 32 bytes */

#define MAXV 256 /* corners of one strip */

static const void *sMdl; /* the model whose chains are read (gBtlStage->mdl), NULL: none yet */

static inline uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }

static inline uint32_t unit(float f) { /* p4_fast's p4_unit: the integer's low byte, 0x80 = 255 */
    int32_t c = f >= 2147483648.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
    c &= 0xFF;
    return c >= 0x80 ? 255u : (uint32_t)c * 2u;
}

static inline const NatMesh *mesh_at(uint32_t chain, int kind) {
    const uint8_t *c = (const uint8_t *)(uintptr_t)chain;
    return rd32(c + 4) == kMagic[kind] ? (const NatMesh *)(c + 8) : NULL;
}

static inline const NatVtx *verts_of(const NatMesh *m) { return (const NatVtx *)((const uint8_t *)m + (NAT_DATA - 8)); }

/* A chain being read: write 0 only checks and measures (every piece written below what is read by then), 1 writes */
typedef struct Conv {
    uint8_t *out;
    int kind, write;
    uint32_t prim, verts, tris, batches;
    float lo[3], hi[3];
    NatVtx ring[2]; /* the two corners before */
    NatVtx *last;   /* the last corner written */
} Conv;

static int put(Conv *c, const NatVtx *v, const uint8_t *read) {
    if (c->out + sizeof(NatVtx) > read) {
        return 0; /* (its place not yet read: none in the game's chains) */
    }
    if (c->write) {
        memcpy(c->out, v, sizeof(NatVtx));
        c->last = (NatVtx *)c->out;
    }
    c->out += sizeof(NatVtx);
    c->verts++;
    return 1;
}

/* The TEX0 a program 8 batch's header writes for context ctx (vu1_native.c hdr_tex0): its register TEX0_1 / TEX0_2,
   or A+D naming it */
static int hdr_tex0(const uint8_t *hdr, int ctx, uint32_t *tex0) {
    uint32_t reg = rd32(hdr + 8) & 15;
    if (reg == (uint32_t)(6 + ctx) || (reg == 14 && (rd32(hdr + 24) & 0xFF) == (uint32_t)(6 + ctx))) {
        tex0[0] = rd32(hdr + 16);
        tex0[1] = rd32(hdr + 20);
        return 1;
    }
    return 0;
}

/* One batch: the strip's tag at +32 of its header, three quadwords a corner (x y z w, r g b a as floats, s t q) */
static int conv_batch(Conv *c, const uint8_t *hdr, const uint8_t *vtx, uint32_t nv) {
    uint64_t tag;
    uint32_t n, p, i, run = 0;

    memcpy(&tag, hdr + 32, 8);
    n = (uint32_t)(tag & 0x7FFF);
    p = (uint32_t)(tag >> 47) & 0x7FF;
    if (n * 3 > nv || n > MAXV || (p & 7) != 4) {
        return 0; /* (not a triangle strip) */
    }
    if (c->kind != K8) {
        if (c->verts != 0 && p != c->prim) {
            return 0; /* (strips of more than one kind) */
        }
        c->prim = p;
    }
    if (n < 3) {
        return 1;
    }
    c->batches++;
    if (c->kind == K8) {
        NatBatch b;
        memset(&b, 0, sizeof(b));
        b.has = (uint32_t)hdr_tex0(hdr, (int)(p >> 9) & 1, b.tex0);
        b.prim = p;
        b.n = n;
        if (c->out + sizeof(b) > hdr + 48) { return 0; }
        if (c->write) { memcpy(c->out, &b, sizeof(b)); }
        c->out += sizeof(b);
    }
    for (i = 0; i < n; i++) {
        const float *f = (const float *)(vtx + i * 48);
        const uint8_t *read = (const uint8_t *)(f + 12);
        float q = f[10] != 0.0f ? f[10] : 1.0f;
        NatVtx v;
        int k, skip = 0;
        if (c->kind == K4 && f[3] != 1.0f) { return 0; }
        if (c->kind == K7) { /* (w: a flag, the triangle that ends here left out: the strip starts again) */
            skip = (*(const uint32_t *)&f[3] & 0xFFFF) != 0;
        }
        v.flags = PVR_CMD_VERTEX;
        v.x = f[0];
        v.y = f[1];
        v.z = f[2];
        v.u = f[8] / q;
        v.v = f[9] / q;
        v.argb = unit(f[7]) << 24 | unit(f[4]) << 16 | unit(f[5]) << 8 | unit(f[6]);
        v.w = c->kind == K8 ? f[3] : 0.0f;
        for (k = 0; k < 3; k++) {
            c->lo[k] = f[k] < c->lo[k] ? f[k] : c->lo[k];
            c->hi[k] = f[k] > c->hi[k] ? f[k] : c->hi[k];
        }
        if (i >= 2 && !skip) {
            if (!run && (!put(c, &c->ring[0], read) || !put(c, &c->ring[1], read))) { return 0; }
            if (!put(c, &v, read)) { return 0; }
            run = 1;
            c->tris++;
        } else if (i >= 2 && run) {
            if (c->write) { c->last->flags = PVR_CMD_VERTEX_EOL; }
            run = 0;
        }
        c->ring[0] = c->ring[1];
        c->ring[1] = v;
    }
    if (run && c->write) {
        c->last->flags = PVR_CMD_VERTEX_EOL;
    }
    return 1;
}

/* A chain read as a file (vu1_native.c walk_vif): one DMA ret tag, its VIF codes; UNPACK V4-32 of a header at 0 and
   the corners behind it, MSCNT runs it; program 4's first batch the object header (one quadword), every other
   batch a tag of one register (program 8: TEX0) and its quadword, the strip's tag, the corners. 0 = not a chain
   this reads (it stays as it is). */
static int chain_walk(uint8_t *chain, Conv *c) {
    uint32_t t0 = rd32(chain), nh = 0, nv = 0, first = c->kind == K4;
    const uint8_t *w = chain + 8, *end = chain + 16 + (t0 & 0xFFFF) * 16, *hdr = NULL, *vtx = NULL;

    if (((t0 >> 28) & 7) != 6 || rd32(chain + 4) != 0) {
        return 0;
    }
    c->out = chain + NAT_DATA;
    c->prim = c->verts = c->tris = c->batches = 0;
    c->lo[0] = c->lo[1] = c->lo[2] = 1e30f;
    c->hi[0] = c->hi[1] = c->hi[2] = -1e30f;
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
        if (first) { /* (program 4's object header: the clipped fans' tag) */
            first = 0;
        } else {
            if (vtx == NULL && nh > 3) { /* the header and the corners in one UNPACK */
                vtx = hdr + 48;
                nv = nh - 3;
                nh = 3;
            }
            if (vtx == NULL || !conv_batch(c, hdr, vtx, nv)) { return 0; }
        }
        hdr = vtx = NULL;
        nh = nv = 0;
    }
    return 1;
}

/* A chain read into its native mesh, in place: checked whole first, then written. 0 = not read (as it was). */
static int chain_read(uint32_t chain, int kind) {
    uint8_t *c = (uint8_t *)(uintptr_t)chain;
    Conv cv;
    NatMesh m;
    float dx, dy, dz;

    memset(&cv, 0, sizeof(cv));
    cv.kind = kind;
    if (!chain_walk(c, &cv)) {
        return 0;
    }
    memset(&m, 0, sizeof(m));
    m.prim = kind == K8 ? cv.batches : cv.prim;
    m.verts = cv.verts;
    m.tris = cv.tris;
    dx = cv.hi[0] - cv.lo[0];
    dy = cv.hi[1] - cv.lo[1];
    dz = cv.hi[2] - cv.lo[2];
    m.cx = (cv.lo[0] + cv.hi[0]) * 0.5f;
    m.cy = (cv.lo[1] + cv.hi[1]) * 0.5f;
    m.cz = (cv.lo[2] + cv.hi[2]) * 0.5f;
    m.r = __builtin_sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f * 1.001f + 1.0f; /* (half the box's diagonal) */
    cv.write = 1;
    chain_walk(c, &cv);
    memcpy(c + 8, &m, sizeof(m));
    *(uint32_t *)(c + 4) = kMagic[kind];
    return 1;
}

/* The stage model's groups as StgModel_Draw reads them (include/battle/stgm_a.h): 7 of {u16, u16 count, mats};
   a material {u16 tex, u16 count, meshes, anim, u16 cur, u16}; a mesh {u16 visIdx, u8, u8 prog, u8 alpha, u8[3],
   u32 chain, mtx}. Groups 0..5 are program 4's (group 2's meshes of prog 0 only); 6 is the debris (program 7). */
typedef struct { uint16_t u0, count; const uint8_t *mats; } Grp;

/* Every mesh's chain read (once: a chain read has its magic, a chain shared by two meshes is read by the first) */
static void stg_load(const void *mdl) {
    const Grp *g = (const Grp *)mdl;
    uint32_t i, j, n = 0, old = 0;
    int gi;
    uint64_t t0 = Gs_NowNs();

    sMdl = mdl;
    if (mdl == NULL) {
        return;
    }
    for (gi = 0; gi < 7; gi++) {
        int kind = gi == 6 ? K7 : K4;
        for (i = 0; i < g[gi].count; i++) {
            const uint8_t *mat = g[gi].mats + i * 16;
            const uint8_t *mesh = *(const uint8_t *const *)(mat + 4);
            for (j = 0; j < *(const uint16_t *)(mat + 2); j++) {
                const uint8_t *e = mesh + j * 16;
                if (gi == 2 && e[3] != 0) { continue; }
                if (mesh_at(rd32(e + 8), kind) != NULL) { continue; }
                if (chain_read(rd32(e + 8), kind)) {
                    n++;
                } else {
                    old++;
                }
            }
        }
    }
    fprintf(stderr, "bt3: stg_native: %u meshes read in %.1f ms; %u left to the old way\n", (unsigned)n,
            (double)(Gs_NowNs() - t0) / 1e6, (unsigned)old);
}

/* ---- each frame ------------------------------------------------------------------------------------------- */

/* program 4's constants (screen, clip, clip -> screen), program 7's and 8's (screen, clip) */
static float sK[48] __attribute__((aligned(8)));
static float sK8[32] __attribute__((aligned(8)));
static unsigned sN[10]; /* TEMP: per 60 frames: meshes fast, cut at the near plane, culled, old way; triangles cut;
                           draws refused; debris drawn, debris culled; nodes drawn, nodes the old way */
static uint64_t sNs;

/* The stage is (re)started (StgModel_InitStage): read again at the next draw */
void StgNative_Forget(void) {
    stg_load(NULL);
}

void StgNative_Begin(const void *mdl, const void *mtx) {
    if (mdl != sMdl) {
        stg_load(mdl);
    }
    Vu0Screen_StoreMtx(sK);
    Vu0Clip_StoreMtx(sK + 16);
    memcpy(sK + 32, mtx, 64);
}

/* m' = what the PVR is given: x' = (x - xo) * sx, y so, z' = z * zk + zo, before the divide by w */
static void fold(float *o, const float *m, const RndDirect *d) {
    int j;
    for (j = 0; j < 4; j++) {
        const float *c = m + 4 * j;
        o[4 * j + 0] = d->sx * (c[0] - d->xo * c[3]);
        o[4 * j + 1] = d->sy * (c[1] - d->yo * c[3]);
        o[4 * j + 2] = d->zk * c[2] + d->zo * c[3];
        o[4 * j + 3] = c[3];
    }
}

/* o = a b (b first; the matrices' columns at 4 j, as XMTRX takes them) */
static void mul(float *o, const float *a, const float *b) {
    int j, r;
    for (j = 0; j < 4; j++) {
        for (r = 0; r < 4; r++) {
            o[4 * j + r] = a[r] * b[4 * j] + a[4 + r] * b[4 * j + 1] + a[8 + r] * b[4 * j + 2] + a[12 + r] * b[4 * j + 3];
        }
    }
}

typedef struct Blast {
    float us, vs, z0, z1;
    uint32_t amask, aor;
} Blast;

static inline float zclamp(float z, const Blast *b) { return z < b->z0 ? b->z0 : z > b->z1 ? b->z1 : z; }

static void blast_of(Blast *b, const RndDirect *d) {
    b->us = d->tex ? d->us : 0.0f;
    b->vs = d->tex ? d->vs : 0.0f;
    b->z0 = d->z0;
    b->z1 = d->z1;
    b->amask = d->fixa >= 0 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    b->aor = d->fixa >= 0 ? (uint32_t)d->fixa << 24 : 0u;
}

/* A mesh wholly in front of the near plane: every corner straight to the TA, the folded screen matrix in XMTRX */
static uint32_t mesh_fast(const NatMesh *m, const Blast *b) {
    const NatVtx *v = verts_of(m);
    uint32_t i, n = m->verts;
    for (i = 0; i < n; i++, v++) {
        shz_vec4_t c = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
        float q = shz_invf(c.w);
        pvr_vertex_t *pv = (pvr_vertex_t *)pvr_dr_target();
        pv->flags = v->flags;
        pv->x = c.x * q;
        pv->y = c.y * q;
        pv->z = zclamp(c.z * q, b);
        pv->u = v->u * b->us;
        pv->v = v->v * b->vs;
        pv->argb = (v->argb & b->amask) | b->aor;
        pv->oargb = 0;
        pvr_dr_commit(pv);
    }
    return n;
}

typedef struct CutV {
    float p[4];  /* clip position */
    float a[6];  /* u, v, a, r, g, b */
} CutV;

static void cut_corner(CutV *o, const float *p, const NatVtx *v) {
    int i;
    for (i = 0; i < 4; i++) { o->p[i] = p[i]; }
    o->a[0] = v->u;
    o->a[1] = v->v;
    o->a[2] = (float)(v->argb >> 24);
    o->a[3] = (float)(v->argb >> 16 & 0xFF);
    o->a[4] = (float)(v->argb >> 8 & 0xFF);
    o->a[5] = (float)(v->argb & 0xFF);
}

/* A triangle that crosses the near plane z = -w (clip space), cut there (one plane: at most four corners), through
   the folded clip -> screen matrix, as a strip of its own */
static uint32_t tri_cut(const CutV *t, const float *toScr, const Blast *b) {
    static RndPvrVtx out[4] __attribute__((aligned(32)));
    static const uint8_t none[4];
    CutV poly[4], *ord[4];
    uint32_t n = 0, i, k;
    for (i = 0; i < 3; i++) {
        const CutV *a = &t[i], *c = &t[(i + 1) % 3];
        float da = a->p[2] + a->p[3], dc = c->p[2] + c->p[3];
        if (da >= 0.0f) { poly[n++] = *a; }
        if ((da >= 0.0f) != (dc >= 0.0f)) {
            float f = da / (da - dc);
            CutV *o = &poly[n++];
            for (k = 0; k < 4; k++) { o->p[k] = a->p[k] + (c->p[k] - a->p[k]) * f; }
            for (k = 0; k < 6; k++) { o->a[k] = a->a[k] + (c->a[k] - a->a[k]) * f; }
        }
    }
    if (n < 3) {
        return 0;
    }
    /* the fan 0, 1, 2 (, 3) as a strip: 0, 1, n - 1, 2 */
    ord[0] = &poly[0];
    ord[1] = &poly[1];
    ord[2] = &poly[n - 1];
    ord[3] = &poly[2];
    shz_xmtrx_load_unaligned_4x4(toScr);
    for (i = 0; i < n; i++) {
        const CutV *c = ord[i];
        shz_vec4_t s = shz_xmtrx_transform_vec4(shz_vec4_init(c->p[0], c->p[1], c->p[2], c->p[3]));
        float q = s.w > 0.0f ? shz_invf(s.w) : 1e6f;
        uint32_t a = (uint32_t)(c->a[2] + 0.5f), r = (uint32_t)(c->a[3] + 0.5f), g = (uint32_t)(c->a[4] + 0.5f), bl = (uint32_t)(c->a[5] + 0.5f);
        out[i].x = s.x * q;
        out[i].y = s.y * q;
        out[i].z = zclamp(s.z * q, b);
        out[i].u = c->a[0] * b->us;
        out[i].v = c->a[1] * b->vs;
        out[i].argb = ((a << 24 | r << 16 | g << 8 | bl) & b->amask) | b->aor;
    }
    Port_DirectSend(out, none, n);
    sN[4]++; /* TEMP */
    return n;
}

/* A mesh across the near plane: its corners in clip space first (which are behind it), then the triangles wholly in
   front sent as runs of the strip (Port_DirectSend), those across it cut (tri_cut; toScr NULL: left out, programs 7
   and 8); XMTRX left as the screen's */
static uint32_t mesh_cut(const NatMesh *m, const float *scr, const float *clip, const float *toScr, const Blast *b) {
    static float cp[MAXV][4] __attribute__((aligned(32)));
    static RndPvrVtx out[MAXV] __attribute__((aligned(32)));
    static uint8_t adc[MAXV], fl[MAXV];
    static uint16_t cut[MAXV];
    const NatVtx *v = verts_of(m), *last = v + m->verts;
    uint32_t i, sent = 0;
    while (v < last) {
        uint32_t n = 1, ncut = 0, any = 0;
        while (v[n - 1].flags != PVR_CMD_VERTEX_EOL) { n++; }
        shz_xmtrx_load_unaligned_4x4(clip);
        for (i = 0; i < n; i++) {
            shz_vec4_t c = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
            cp[i][0] = c.x;
            cp[i][1] = c.y;
            cp[i][2] = c.z;
            cp[i][3] = c.w;
            fl[i] = c.z < -c.w || c.w <= 0.0f;
            adc[i] = 0;
            if (i >= 2) {
                uint32_t f = fl[i - 2] | fl[i - 1] | fl[i];
                if (f) {
                    adc[i] = 1;
                    if (toScr != NULL && !(fl[i - 2] & fl[i - 1] & fl[i])) { cut[ncut++] = (uint16_t)i; }
                } else {
                    any = 1;
                }
            }
        }
        if (any) {
            shz_xmtrx_load_unaligned_4x4(scr);
            for (i = 0; i < n; i++) {
                shz_vec4_t c;
                float q;
                if (fl[i]) { continue; } /* (in no triangle sent) */
                c = shz_xmtrx_transform_vec4(shz_vec4_init(v[i].x, v[i].y, v[i].z, 1.0f));
                q = shz_invf(c.w);
                out[i].x = c.x * q;
                out[i].y = c.y * q;
                out[i].z = zclamp(c.z * q, b);
                out[i].u = v[i].u * b->us;
                out[i].v = v[i].v * b->vs;
                out[i].argb = (v[i].argb & b->amask) | b->aor;
            }
            Port_DirectSend(out, adc, n);
            sent += n;
        }
        for (i = 0; i < ncut; i++) {
            CutV t[3];
            uint32_t k = cut[i], j;
            for (j = 0; j < 3; j++) { cut_corner(&t[j], cp[k - 2 + j], &v[k - 2 + j]); }
            sent += tri_cut(t, toScr, b);
        }
        v += n;
    }
    shz_xmtrx_load_unaligned_4x4(scr);
    return sent;
}

/* plane (a, b, c, d) of a clip matrix: a * x + b * y + c * z + d >= 0 inside, with |(a, b, c)| = 1 */
static void plane(float *o, const float *m, int i, float si, int j, float sj) {
    float l;
    int r;
    for (r = 0; r < 4; r++) { o[r] = m[4 * r + i] * si + (j >= 0 ? m[4 * r + j] * sj : 0.0f); }
    l = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
    l = l > 0.0f ? 1.0f / __builtin_sqrtf(l) : 0.0f;
    for (r = 0; r < 4; r++) { o[r] *= l; }
}

/* the frustum's planes of clip matrix m: w + z (near), w (the eye), w -+ x, w -+ y */
static void planes(float (*pl)[4], const float *m) {
    plane(pl[0], m, 3, 1.0f, 2, 1.0f);
    plane(pl[1], m, 3, 1.0f, -1, 0.0f);
    plane(pl[2], m, 3, 1.0f, 0, -1.0f);
    plane(pl[3], m, 3, 1.0f, 0, 1.0f);
    plane(pl[4], m, 3, 1.0f, 1, -1.0f);
    plane(pl[5], m, 3, 1.0f, 1, 1.0f);
}

/* 0 wholly outside, 1 wholly in front of the near plane, 2 across it */
static int sphere(const float (*pl)[4], const NatMesh *m) {
    int p, cross = 0;
    for (p = 0; p < 6; p++) {
        float e = pl[p][0] * m->cx + pl[p][1] * m->cy + pl[p][2] * m->cz + pl[p][3];
        if (e < -m->r) { return 0; }
        if (p < 2 && e < m->r) { cross = 1; }
    }
    return 1 + cross;
}

/* The polygon of the state the draw was queued in, its texture tex0 (0: none): Rnd_Direct's 1, 2 or 0 (said once) */
static int head(RndDirect *d, uint32_t prim, uint64_t tex0, uint32_t tris, uint32_t verts) {
    RndPoly h;
    int r;
    memset(&h, 0, sizeof(h));
    h.gsTex0 = (prim >> 4) & 1 ? tex0 : 0;
    h.st.flags = RND_VU;
    r = Rnd_Direct(d, &h, (int)(prim >> 9) & 1, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1, tris, verts + verts / 4 + 16);
    if (r == 0) { /* (a state the PVR path does not take: the chain is no longer there for the old way) */
        static int told;
        if (!told++) { fprintf(stderr, "bt3: stg_native: a draw whose state the PVR path does not take: not drawn\n"); }
        sN[5]++; /* TEMP */
    }
    return r;
}

/* A draw queued where the list has it: the state and its textures' handles; then the kind's own */
typedef struct RecHead {
    RndSnap st;
    RndTexNote note[8];
    uint32_t notes, prim, n, pad;
} RecHead;

static void rec_take(RecHead *h, uint32_t prim) {
    uint32_t ctx = (prim >> 9) & 1;
    Rnd_SnapTake(&h->st);
    h->prim = prim;
    h->notes = 0;
    if ((prim >> 4) & 1 && Rnd_Tex0((int)ctx) != 0) {
        Port_TexNote(&h->note[0], Rnd_Tex0((int)ctx));
        h->notes = 1;
    }
}

static void rec_open(const RecHead *h, RndSnap *now) {
    Rnd_SnapTake(now);
    Rnd_SnapPut(&h->st);
    Port_TexNotes(h->notes ? h->note : NULL, h->notes);
}

static void rec_close(const RndSnap *now) {
    Port_TexNotes(NULL, 0);
    Rnd_SnapPut(now);
}

/* Memory for a draw: in the list's queue, or (not deferred, or no memory) a buffer of its own, made now */
static void *rec_new(uint32_t bytes, void (*fn)(const void *), int *now) {
    static void *own;
    static uint32_t ownBytes;
    void *r = Gs_Defers() ? Gs_DeferPush((bytes + 7) & ~7u, fn, Port_Addr(gDmaCur)) : NULL;
    *now = r == NULL;
    if (r == NULL && bytes > ownBytes) {
        free(own);
        own = malloc(bytes);
        ownBytes = own != NULL ? bytes : 0;
    }
    return r != NULL ? r : bytes <= ownBytes ? own : NULL;
}

/* ---- groups 0..5 (program 4) and the debris (program 7): a material's meshes, one draw ----------------------- */

typedef struct Ent7 {
    const NatMesh *m;
    float alpha, pad[2];
    float l[16]; /* the mesh's matrix (its node's) */
} Ent7;

typedef struct Rec4 {
    RecHead h;
    float k[48];
    const NatMesh *m[];
} Rec4;

typedef struct Rec7 {
    RecHead h;
    float k[32];
    Ent7 e[];
} Rec7;

#define PEND_MAX 512
static const NatMesh *sPend[PEND_MAX];
static Ent7 sPend7[PEND_MAX / 4];
static uint32_t sPendN, sPendPrim;
static int sPendKind;

static void rec4_draw(const void *blob) {
    const Rec4 *rec = (const Rec4 *)blob;
    static const NatMesh *keep[PEND_MAX];
    static uint8_t how[PEND_MAX];
    uint32_t i, tris = 0, verts = 0, sent = 0, nf = 0;
    float pl[6][4], scr[16], toScr[16];
    RndSnap now;
    RndDirect d;
    Blast b;
    uint64_t t0 = Gs_NowNs();

    planes(pl, rec->k + 16);
    for (i = 0; i < rec->h.n; i++) {
        int s = sphere((const float (*)[4])pl, rec->m[i]);
        if (s == 0) {
            sN[2]++; /* TEMP */
            continue;
        }
        keep[nf] = rec->m[i];
        how[nf++] = (uint8_t)(s == 2);
        tris += rec->m[i]->tris;
        verts += rec->m[i]->verts;
    }
    if (nf != 0) {
        rec_open(&rec->h, &now);
        if (head(&d, rec->h.prim, Rnd_Tex0((int)(rec->h.prim >> 9) & 1), tris, verts) == 1) {
            blast_of(&b, &d);
            fold(scr, rec->k, &d);
            fold(toScr, rec->k + 32, &d);
            shz_xmtrx_load_unaligned_4x4(scr);
            for (i = 0; i < nf; i++) {
                if (how[i]) {
                    sent += mesh_cut(keep[i], scr, rec->k + 16, toScr, &b);
                    sN[1]++; /* TEMP */
                } else {
                    sent += mesh_fast(keep[i], &b);
                    sN[0]++; /* TEMP */
                }
            }
            gStatVtx += sent;
        }
        rec_close(&now);
    }
    sNs += Gs_NowNs() - t0;
}

/* each mesh with its own matrix (S L, C L) and alpha, the triangles across the near plane left out */
static void rec7_draw(const void *blob) {
    const Rec7 *rec = (const Rec7 *)blob;
    uint32_t i, tris = 0, verts = 0, sent = 0;
    RndSnap now;
    RndDirect d;
    Blast b;
    uint64_t t0 = Gs_NowNs();

    for (i = 0; i < rec->h.n; i++) {
        tris += rec->e[i].m->tris;
        verts += rec->e[i].m->verts;
    }
    rec_open(&rec->h, &now);
    if (head(&d, rec->h.prim, Rnd_Tex0((int)(rec->h.prim >> 9) & 1), tris, verts) == 1) {
        blast_of(&b, &d);
        for (i = 0; i < rec->h.n; i++) {
            const Ent7 *e = &rec->e[i];
            float sl[16], cl[16], pl[6][4];
            int s;
            mul(cl, rec->k + 16, e->l);
            planes(pl, cl);
            if ((s = sphere((const float (*)[4])pl, e->m)) == 0) {
                sN[7]++; /* TEMP */
                continue;
            }
            mul(sl, rec->k, e->l);
            {
                float f[16];
                Blast be = b;
                if (d.fixa < 0) { /* (the corners' alpha: the mesh's) */
                    be.amask = 0x00FFFFFFu;
                    be.aor = unit(e->alpha) << 24;
                }
                fold(f, sl, &d);
                shz_xmtrx_load_unaligned_4x4(f);
                sent += s == 2 ? mesh_cut(e->m, f, cl, NULL, &be) : mesh_fast(e->m, &be);
            }
            sN[6]++; /* TEMP */
        }
        gStatVtx += sent;
    }
    rec_close(&now);
    sNs += Gs_NowNs() - t0;
}

/* The material's meshes so far, one draw */
void StgNative_End(void) {
    int now;
    uint32_t n = sPendN;

    if (n == 0) {
        return;
    }
    sPendN = 0;
    if (gPortResim || gsSkipFrame) { /* (a tick without a picture) */
        return;
    }
    if (sPendKind == K4) {
        Rec4 *rec = (Rec4 *)rec_new((uint32_t)(sizeof(Rec4) + n * sizeof(NatMesh *)), rec4_draw, &now);
        if (rec == NULL) { return; }
        rec_take(&rec->h, sPendPrim);
        rec->h.n = n;
        memcpy(rec->k, sK, sizeof(rec->k));
        memcpy(rec->m, sPend, n * sizeof(NatMesh *));
        if (now) { rec4_draw(rec); }
    } else {
        Rec7 *rec = (Rec7 *)rec_new((uint32_t)(sizeof(Rec7) + n * sizeof(Ent7)), rec7_draw, &now);
        if (rec == NULL) { return; }
        rec_take(&rec->h, sPendPrim);
        rec->h.n = n;
        memcpy(rec->k, sK8, sizeof(rec->k));
        memcpy(rec->e, sPend7, n * sizeof(Ent7));
        if (now) { rec7_draw(rec); }
    }
}

static void pend_kind(int kind, uint32_t prim, uint32_t max) {
    if (sPendN != 0 && (sPendKind != kind || prim != sPendPrim || sPendN == max)) {
        StgNative_End();
    }
    sPendKind = kind;
    sPendPrim = prim;
}

/* A visible mesh of the material being drawn (Vu1Pkt_CallProg4's chain) */
void StgNative_Mesh(uint32_t chain) {
    const NatMesh *m = mesh_at(chain, K4);
    if (m == NULL) { /* (not read: the old way, its constants as Vu1Pkt_LoadProg4 would have set them) */
        StgNative_End();
        Vu1Native_SetProg4(sK);
        if (!Vu1Native_Prog4((const void *)(uintptr_t)chain)) {
            static int told;
            if (!told++) { fprintf(stderr, "bt3: stg_native: a mesh neither read nor taken by vu1_native: not drawn\n"); }
        }
        sN[3]++;
        return;
    }
    pend_kind(K4, m->prim, PEND_MAX);
    sPend[sPendN++] = m;
}

/* A visible piece of debris (Vu1Pkt_CallProg7's chain, its matrix as Vu0Cur_LoadMtx loaded it, its alpha) */
void StgNative_Mesh7(const void *mdl, uint32_t chain, const void *mtx, float alpha) {
    const NatMesh *m;
    Ent7 *e;
    if (mdl != sMdl) {
        stg_load(mdl);
    }
    Vu0Screen_StoreMtx(sK8);
    Vu0Clip_StoreMtx(sK8 + 16);
    if ((m = mesh_at(chain, K7)) == NULL) { /* (not read: the old way, the constants Vu1Pkt_CallProg7 would send) */
        static float k[52] __attribute__((aligned(16)));
        StgNative_End();
        memcpy(k, mtx, 64);
        memcpy(k + 16, sK8, 128);
        k[51] = alpha;
        Vu1Native_Prog7((const void *)(uintptr_t)chain, k);
        sN[3]++;
        return;
    }
    if (m->verts == 0) {
        return;
    }
    pend_kind(K7, m->prim, PEND_MAX / 4);
    e = &sPend7[sPendN++];
    e->m = m;
    e->alpha = alpha;
    memcpy(e->l, mtx, 64);
}

/* ---- the animated objects (program 8): a node, one draw --------------------------------------------------- */

typedef struct Rec8 {
    RecHead h;
    const NatMesh *m;
    uint32_t pad[1];
    float s[16], c[16], a[16], b[16]; /* screen, clip; the node's matrix and its parent's, their offsets in */
} Rec8;

/* m with its offset o taken first: m (p - o) = m' p */
static void shifted(float *o, const float *m, const float *ofs) {
    int r;
    memcpy(o, m, 64);
    for (r = 0; r < 4; r++) { o[12 + r] = m[12 + r] - (m[r] * ofs[0] + m[4 + r] * ofs[1] + m[8 + r] * ofs[2]); }
}

/* each batch: its TEX0's polygon, each corner A' p and B' p blended by its weight (as program 8: B + (A - B) w), then
   screen and clip; a triangle with a corner outside the clip volume (vu1_native.c clip_flags) left out */
static void rec8_draw(const void *blob) {
    const Rec8 *rec = (const Rec8 *)blob;
    static RndPvrVtx out[MAXV] __attribute__((aligned(32)));
    static uint8_t adc[MAXV];
    const uint8_t *p = (const uint8_t *)rec->m + (NAT_DATA - 8);
    uint32_t bi, i, sent = 0;
    RndSnap now;
    uint64_t t0 = Gs_NowNs();

    rec_open(&rec->h, &now);
    for (bi = 0; bi < rec->m->prim; bi++) {
        const NatBatch *nb = (const NatBatch *)p;
        const NatVtx *v = (const NatVtx *)(p + sizeof(NatBatch));
        uint32_t n = nb->n, ctx = (nb->prim >> 9) & 1, f0 = 0, f1 = 0, f2 = 0, any = 0;
        uint64_t tex0 = nb->has ? (uint64_t)nb->tex0[1] << 32 | nb->tex0[0] : Rnd_Tex0((int)ctx);
        float sf[16];
        RndDirect d;
        Blast b;
        p += sizeof(NatBatch) + n * sizeof(NatVtx);
        for (i = 0; i < n; i++) { /* the corners, in clip space and on the screen at once */
            const float *a = rec->a, *m = rec->b;
            float x = v[i].x, y = v[i].y, z = v[i].z, w = v[i].w, q[3], c[4];
            int r;
            for (r = 0; r < 3; r++) {
                float pa = a[r] * x + a[4 + r] * y + a[8 + r] * z + a[12 + r];
                float pb = m[r] * x + m[4 + r] * y + m[8 + r] * z + m[12 + r];
                q[r] = pb + (pa - pb) * w;
            }
            for (r = 0; r < 4; r++) { c[r] = rec->c[r] * q[0] + rec->c[4 + r] * q[1] + rec->c[8 + r] * q[2] + rec->c[12 + r]; }
            out[i].x = q[0]; /* (the blended position, to the screen below) */
            out[i].y = q[1];
            out[i].z = q[2];
            f0 = f1;
            f1 = f2;
            f2 = c[0] > 4.0f * c[3] || c[0] < -4.0f * c[3] || c[1] > 4.0f * c[3] || c[1] < -4.0f * c[3] || c[2] < -c[3] || c[3] <= 0.0f;
            adc[i] = i >= 2 && (f0 | f1 | f2);
            any |= i >= 2 && !adc[i];
        }
        if (!any) {
            continue;
        }
        if (head(&d, nb->prim, tex0, n - 2, n) != 1) {
            continue;
        }
        blast_of(&b, &d);
        fold(sf, rec->s, &d);
        shz_xmtrx_load_unaligned_4x4(sf);
        for (i = 0; i < n; i++) {
            shz_vec4_t s = shz_xmtrx_transform_vec4(shz_vec4_init(out[i].x, out[i].y, out[i].z, 1.0f));
            float q = s.w > 0.0f ? shz_invf(s.w) : 1.0f; /* (behind the eye: in no triangle sent) */
            out[i].x = s.x * q;
            out[i].y = s.y * q;
            out[i].z = zclamp(s.z * q, &b);
            out[i].u = v[i].u * b.us;
            out[i].v = v[i].v * b.vs;
            out[i].argb = (v[i].argb & b.amask) | b.aor;
        }
        Port_DirectSend(out, adc, n);
        sent += n;
    }
    gStatVtx += sent;
    rec_close(&now);
    sNs += Gs_NowNs() - t0;
}

/* An animated node's mesh (Vu1Node_Draw: its chain, its matrix and its parent's, their offsets), with the screen and
   clip matrices DrawAnims stored */
void StgNative_Mesh8(uint32_t chain, const float *world, const float *parent, const float *ofsA, const float *ofsB) {
    const NatMesh *m = mesh_at(chain, K8);
    const uint8_t *p;
    Rec8 *rec;
    uint32_t bi;
    int now;

    StgNative_End();
    if (m == NULL && chain_read(chain, K8)) {
        m = mesh_at(chain, K8);
    }
    if (m == NULL) { /* (not read: the old way, the node's constants as Vu1Node_Draw makes them) */
        static float k[40] __attribute__((aligned(16)));
        memcpy(k, world, 64);
        memcpy(k + 16, parent, 64);
        memcpy(k + 32, ofsA, 16);
        memcpy(k + 36, ofsB, 16);
        Vu1Native_Prog8((const void *)(uintptr_t)chain, k);
        sN[9]++; /* TEMP */
        return;
    }
    if (gPortResim || gsSkipFrame || m->prim == 0) {
        return;
    }
    if ((rec = (Rec8 *)rec_new(sizeof(Rec8), rec8_draw, &now)) == NULL) {
        return;
    }
    Rnd_SnapTake(&rec->h.st);
    rec->h.notes = 0;
    p = (const uint8_t *)m + (NAT_DATA - 8);
    for (bi = 0; bi < m->prim; bi++) { /* its batches' textures' handles, as they are now */
        const NatBatch *nb = (const NatBatch *)p;
        uint32_t ctx = (nb->prim >> 9) & 1, k;
        uint64_t tex0 = nb->has ? (uint64_t)nb->tex0[1] << 32 | nb->tex0[0] : Rnd_Tex0((int)ctx);
        p += sizeof(NatBatch) + nb->n * sizeof(NatVtx);
        if (!((nb->prim >> 4) & 1) || tex0 == 0) { continue; }
        for (k = 0; k < rec->h.notes && rec->h.note[k].tex0 != tex0; k++) {
        }
        if (k == rec->h.notes && k < 8) {
            Port_TexNote(&rec->h.note[rec->h.notes++], tex0);
        }
    }
    rec->m = m;
    Vu0Screen_StoreMtx(rec->s);
    Vu0Clip_StoreMtx(rec->c);
    shifted(rec->a, world, ofsA);
    shifted(rec->b, parent, ofsB);
    sN[8]++; /* TEMP */
    if (now) { rec8_draw(rec); }
}

/* TEMP: what the stage cost, every 60 frames (called at the stage's end) */
void StgNative_Frame(void) {
    static unsigned from;
    if (gGsFrame - from >= 60) {
        unsigned f = gGsFrame - from ? gGsFrame - from : 1;
        fprintf(stderr, "stg_native per frame: %.1f ms; meshes fast %u, near-cut %u, culled %u, old way %u; triangles cut %u; "
                        "draws refused %u; debris %u (culled %u); nodes %u (old way %u)\n", (double)sNs / 1e6 / f, sN[0] / f,
                sN[1] / f, sN[2] / f, sN[3] / f, sN[4] / f, sN[5] / f, sN[6] / f, sN[7] / f, sN[8] / f, sN[9] / f);
        memset(sN, 0, sizeof(sN));
        sNs = 0;
        from = gGsFrame;
    }
}
#endif

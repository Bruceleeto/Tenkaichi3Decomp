/* The Dreamcast's stage model drawn natively (src/battle/stgm_a.c StgModel_Draw, its groups 0..5: VU1 program 4 on the
   PS2). No VU1, no VIF and no GIF tags at run time:
   - at the stage's first draw (StgNative_Begin) each mesh's chain is read once, as a file, and becomes in its own
     memory PVR-ready vertices (position, s / q and t / q, the colour as ARGB) and a bounding sphere: no memory more;
   - each frame StgModel_Draw hands over the matrices (StgNative_Begin, in place of Vu1Pkt_LoadProg4) and every visible
     mesh (StgNative_Mesh, in place of Vu1Pkt_CallProg4); each material's meshes become one draw (StgNative_End), made
     where the display list has it (Gs_DeferPush), as the other draws are;
   - a draw: one polygon header for the material (Rnd_Direct, the texture StgModel_SetTexture bound), each mesh's
     sphere against the frustum, then its corners straight to the TA by store queue: one ftrv with the screen matrix
     and the PVR's mapping in it, one reciprocal. A mesh that crosses the near plane goes the same way with its
     triangles there cut at it (as DMS's dc_model.c): the TA takes corners off the screen's sides.
   A mesh whose chain is not read goes the old way (port/src/vu1_native.c); a material the PVR path does not take
   (Rnd_Direct 0) is not drawn (said once). */
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
extern void Vu0Screen_StoreMtx(void *m); /* src/sys: what Vu1Pkt_LoadProg4 hands program 4 */
extern void Vu0Clip_StoreMtx(void *m);

/* A mesh once read, in place of its chain (the chain's own memory: a corner is 48 bytes there, 32 here):
     chain + 0  the DMA tag's first word, as it was
     chain + 4  NAT_MAGIC (a ret tag's address word, 0 in the game's chains)
     chain + 8  NatMesh
     chain + 48 the corners, as the PVR takes them but for the position (PCW: VERTEX, or VERTEX_EOL on a strip's last) */
#define NAT_MAGIC 0x5654414Eu /* "NATV" */
#define NAT_VTX 48

typedef struct NatVtx {
    uint32_t flags;
    float x, y, z, u, v;
    uint32_t argb, pad;
} NatVtx; /* 32 bytes */

typedef struct NatMesh {
    uint32_t prim;       /* its strips' PRIM (one for all of them) */
    uint32_t verts, tris, pad;
    float cx, cy, cz, r; /* the bounding sphere */
} NatMesh; /* 32 bytes */

#define MAXV 256 /* corners of one strip */

static const void *sMdl; /* the model whose chains are read (gBtlStage->mdl), NULL: none yet */

static inline uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }

static inline uint32_t unit(float f) { /* p4_fast's p4_unit: the integer's low byte, 0x80 = 255 */
    int32_t c = f >= 2147483648.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
    c &= 0xFF;
    return c >= 0x80 ? 255u : (uint32_t)c * 2u;
}

static inline const NatMesh *mesh_at(uint32_t chain) {
    const uint8_t *c = (const uint8_t *)(uintptr_t)chain;
    return rd32(c + 4) == NAT_MAGIC ? (const NatMesh *)(c + 8) : NULL;
}

static inline const NatVtx *verts_of(const NatMesh *m) { return (const NatVtx *)((const uint8_t *)m + (NAT_VTX - 8)); }

/* A program 4 chain read as a file (vu1_native.c walk_vif / p4_batch's layout): one DMA ret tag, its VIF codes; UNPACK
   V4-32 of a header at 0 and the corners behind it, MSCNT runs it; the first batch the object header, every other a
   NOP tag and its quadword, the strip's GIF tag, three quadwords a corner (x y z w, r g b a as floats, s t q).
   write 0: only checked and measured (m), every corner written below where it is read; write 1: the corners written,
   each where the chain had already been read. 0 = not a chain this reads (the mesh stays on the old path). */
static int chain_read(uint8_t *chain, NatMesh *m, int write) {
    uint32_t t0 = rd32(chain), nh = 0, nv = 0, first = 1, prim = 0, nc = 0, tris = 0, ns = 0;
    const uint8_t *w = chain + 8, *end = chain + 16 + (t0 & 0xFFFF) * 16, *hdr = NULL, *vtx = NULL;
    NatVtx *out = (NatVtx *)(chain + NAT_VTX);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};

    if (((t0 >> 28) & 7) != 6 || rd32(chain + 4) != 0) {
        return 0;
    }
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
        if (first) { /* the object header (the clipped fans' tag) */
            first = 0;
        } else {
            uint64_t tag;
            uint32_t n, p, i;
            if (vtx == NULL && nh > 3) { /* the header and the corners in one UNPACK */
                vtx = hdr + 48;
                nv = nh - 3;
                nh = 3;
            }
            if (vtx == NULL) { return 0; }
            memcpy(&tag, hdr + 32, 8);
            n = (uint32_t)(tag & 0x7FFF);
            p = (uint32_t)(tag >> 47) & 0x7FF;
            if (n * 3 > nv || n > MAXV || (ns != 0 && p != prim) || (p & 7) != 4) {
                return 0; /* (not a triangle strip, or strips of more than one kind) */
            }
            prim = p;
            ns++;
            if (n >= 3) {
                for (i = 0; i < n; i++) {
                    const float *c = (const float *)(vtx + i * 48);
                    float x = c[0], y = c[1], z = c[2], q = c[10] != 0.0f ? c[10] : 1.0f;
                    float u = c[8] / q, v = c[9] / q;
                    uint32_t argb = unit(c[7]) << 24 | unit(c[4]) << 16 | unit(c[5]) << 8 | unit(c[6]);
                    if (!write) {
                        int k;
                        if (c[3] != 1.0f || (const uint8_t *)&out[nc + i + 1] > (const uint8_t *)(c + 12)) {
                            return 0; /* (a corner whose place is not yet read: none in the game's chains) */
                        }
                        for (k = 0; k < 3; k++) {
                            lo[k] = c[k] < lo[k] ? c[k] : lo[k];
                            hi[k] = c[k] > hi[k] ? c[k] : hi[k];
                        }
                    } else { /* (the corner read whole first: its place overlaps it) */
                        NatVtx *o = &out[nc + i];
                        o->flags = i + 1 == n ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                        o->x = x;
                        o->y = y;
                        o->z = z;
                        o->u = u;
                        o->v = v;
                        o->argb = argb;
                        o->pad = 0;
                    }
                }
                nc += n;
                tris += n - 2;
            }
        }
        hdr = vtx = NULL;
        nh = nv = 0;
    }
    if (nc == 0) {
        return 0;
    }
    if (!write) {
        float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
        m->prim = prim;
        m->verts = nc;
        m->tris = tris;
        m->pad = 0;
        m->cx = (lo[0] + hi[0]) * 0.5f;
        m->cy = (lo[1] + hi[1]) * 0.5f;
        m->cz = (lo[2] + hi[2]) * 0.5f;
        m->r = __builtin_sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f * 1.001f + 1.0f; /* (half the box's diagonal) */
    }
    return 1;
}

/* The stage model's groups as StgModel_Draw reads them (include/battle/stgm_a.h): 7 of {u16, u16 count, mats};
   a material {u16 tex, u16 count, meshes, anim, u16 cur, u16}; a mesh {u16 visIdx, u8, u8 prog, u8 alpha, u8[3],
   u32 chain, mtx}. Groups 0..5 are program 4's (group 2's meshes of prog 0 only); 6 is the debris (program 7). */
typedef struct { uint16_t u0, count; const uint8_t *mats; } Grp;

/* Every mesh's chain read into its native mesh, in place (once: a chain read has NAT_MAGIC, a chain shared by two
   meshes is read by the first) */
static void stg_load(const void *mdl) {
    const Grp *g = (const Grp *)mdl;
    uint32_t i, j, n = 0, old = 0, verts = 0;
    int gi;
    uint64_t t0 = Gs_NowNs();

    sMdl = mdl;
    if (mdl == NULL) {
        return;
    }
    for (gi = 0; gi < 6; gi++) {
        for (i = 0; i < g[gi].count; i++) {
            const uint8_t *mat = g[gi].mats + i * 16;
            const uint8_t *mesh = *(const uint8_t *const *)(mat + 4);
            for (j = 0; j < *(const uint16_t *)(mat + 2); j++) {
                const uint8_t *e = mesh + j * 16;
                uint8_t *c = (uint8_t *)(uintptr_t)rd32(e + 8);
                NatMesh m;
                if (gi == 2 && e[3] != 0) { continue; }
                if (mesh_at(rd32(e + 8)) != NULL) { continue; }
                if (!chain_read(c, &m, 0)) {
                    old++;
                    continue;
                }
                chain_read(c, &m, 1);
                memcpy(c + 8, &m, sizeof(m));
                *(uint32_t *)(c + 4) = NAT_MAGIC;
                n++;
                verts += m.verts;
            }
        }
    }
    fprintf(stderr, "bt3: stg_native: %u meshes read (%u corners) in %.1f ms; %u left to the old way\n", (unsigned)n,
            (unsigned)verts, (double)(Gs_NowNs() - t0) / 1e6, (unsigned)old);
}

/* ---- each frame ------------------------------------------------------------------------------------------- */

/* A material's draw, queued where the list has it: program 4's constants (screen, clip, clip -> screen), the state,
   its texture's handles, the meshes. */
typedef struct StgRec {
    float k[48];
    RndSnap st;
    RndTexNote note;
    uint32_t prim, notes, n, pad;
    const NatMesh *m[];
} StgRec;

#define PEND_MAX 512
static float sK[48] __attribute__((aligned(8)));
static const NatMesh *sPend[PEND_MAX];
static uint32_t sPendN, sPendPrim;
static unsigned sN[6]; /* TEMP: per 60 frames: meshes fast, cut at the near plane, culled, old path; triangles cut;
                          materials Rnd_Direct did not take */
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

typedef struct Blast {
    float us, vs, z0, z1;
    uint32_t amask, aor;
} Blast;

static inline float zclamp(float z, const Blast *b) { return z < b->z0 ? b->z0 : z > b->z1 ? b->z1 : z; }

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
   front sent as runs of the strip (Port_DirectSend), those across it cut (tri_cut); XMTRX left as the screen's */
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
                    if (!(fl[i - 2] & fl[i - 1] & fl[i])) { cut[ncut++] = (uint16_t)i; }
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

/* plane (a, b, c, d) of the clip matrix: a * x + b * y + c * z + d >= 0 inside, with |(a, b, c)| = 1 */
static void plane(float *o, const float *m, int i, float si, int j, float sj) {
    float l;
    int r;
    for (r = 0; r < 4; r++) { o[r] = m[4 * r + i] * si + (j >= 0 ? m[4 * r + j] * sj : 0.0f); }
    l = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
    l = l > 0.0f ? 1.0f / __builtin_sqrtf(l) : 0.0f;
    for (r = 0; r < 4; r++) { o[r] *= l; }
}

static void rec_draw(const void *blob) {
    const StgRec *rec = (const StgRec *)blob;
    uint32_t prim = rec->prim, ctx = (prim >> 9) & 1, i, tris = 0, verts = 0, sent = 0, nf = 0;
    static const NatMesh *keep[PEND_MAX];
    static uint8_t how[PEND_MAX];
    float pl[6][4], scr[16], toScr[16];
    RndSnap now;
    RndPoly h;
    RndDirect d;
    Blast b;
    int r;
    uint64_t t0 = Gs_NowNs();

    /* the frustum's planes: w + z (near), w (the eye), w -+ x, w -+ y */
    plane(pl[0], rec->k + 16, 3, 1.0f, 2, 1.0f);
    plane(pl[1], rec->k + 16, 3, 1.0f, -1, 0.0f);
    plane(pl[2], rec->k + 16, 3, 1.0f, 0, -1.0f);
    plane(pl[3], rec->k + 16, 3, 1.0f, 0, 1.0f);
    plane(pl[4], rec->k + 16, 3, 1.0f, 1, -1.0f);
    plane(pl[5], rec->k + 16, 3, 1.0f, 1, 1.0f);
    for (i = 0; i < rec->n; i++) {
        const NatMesh *m = rec->m[i];
        int p, cross = 0, out = 0;
        for (p = 0; p < 6 && !out; p++) {
            float e = pl[p][0] * m->cx + pl[p][1] * m->cy + pl[p][2] * m->cz + pl[p][3];
            if (e < -m->r) { out = 1; }
            if (p < 2 && e < m->r) { cross = 1; }
        }
        if (out) {
            sN[2]++; /* TEMP */
            continue;
        }
        keep[nf] = m;
        how[nf++] = (uint8_t)cross;
        tris += m->tris;
        verts += m->verts;
    }
    if (nf == 0) {
        sNs += Gs_NowNs() - t0;
        return;
    }
    Rnd_SnapTake(&now);
    Rnd_SnapPut(&rec->st);
    Port_TexNotes(rec->notes ? &rec->note : NULL, rec->notes);
    memset(&h, 0, sizeof(h));
    h.gsTex0 = (prim >> 4) & 1 ? Rnd_Tex0((int)ctx) : 0;
    h.st.flags = RND_VU;
    r = Rnd_Direct(&d, &h, (int)ctx, (int)(prim >> 6) & 1, (int)(prim >> 3) & 1, tris, verts + verts / 4 + 16);
    if (r == 0) { /* (a state the PVR path does not take: the chain is no longer there for the old way) */
        static int told;
        if (!told++) { fprintf(stderr, "bt3: stg_native: a material whose state the PVR path does not take: not drawn\n"); }
        sN[5]++; /* TEMP */
    } else if (r == 1) {
        b.us = d.tex ? d.us : 0.0f;
        b.vs = d.tex ? d.vs : 0.0f;
        b.z0 = d.z0;
        b.z1 = d.z1;
        b.amask = d.fixa >= 0 ? 0x00FFFFFFu : 0xFFFFFFFFu;
        b.aor = d.fixa >= 0 ? (uint32_t)d.fixa << 24 : 0u;
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
    Port_TexNotes(NULL, 0);
    Rnd_SnapPut(&now);
    sNs += Gs_NowNs() - t0;
}

/* The material's meshes so far, one draw */
void StgNative_End(void) {
    uint32_t bytes, ctx;
    StgRec *rec;
    static StgRec *now;

    if (sPendN == 0) {
        return;
    }
    if (gPortResim || gsSkipFrame) { /* (a tick without a picture) */
        sPendN = 0;
        return;
    }
    bytes = (uint32_t)((sizeof(StgRec) + sPendN * sizeof(NatMesh *) + 7) & ~7u);
    rec = Gs_Defers() ? (StgRec *)Gs_DeferPush(bytes, rec_draw, Port_Addr(gDmaCur)) : NULL;
    if (rec == NULL) { /* (not deferred, or no memory: made now) */
        if (now == NULL && (now = (StgRec *)malloc(sizeof(StgRec) + PEND_MAX * sizeof(NatMesh *))) == NULL) {
            sPendN = 0;
            return;
        }
        rec = now;
    }
    memcpy(rec->k, sK, sizeof(sK));
    Rnd_SnapTake(&rec->st);
    rec->prim = sPendPrim;
    ctx = (sPendPrim >> 9) & 1;
    rec->notes = 0;
    if ((sPendPrim >> 4) & 1 && Rnd_Tex0((int)ctx) != 0) {
        Port_TexNote(&rec->note, Rnd_Tex0((int)ctx));
        rec->notes = 1;
    }
    rec->n = sPendN;
    memcpy(rec->m, sPend, sPendN * sizeof(NatMesh *));
    sPendN = 0;
    if (rec == now) {
        rec_draw(rec);
    }
}

/* A visible mesh of the material being drawn (Vu1Pkt_CallProg4's chain) */
void StgNative_Mesh(uint32_t chain) {
    const NatMesh *m = mesh_at(chain);
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
    if (sPendN == PEND_MAX || (sPendN != 0 && m->prim != sPendPrim)) {
        StgNative_End();
    }
    sPendPrim = m->prim;
    sPend[sPendN++] = m;
}

/* TEMP: what the stage cost, every 60 frames (called at the stage's end) */
void StgNative_Frame(void) {
    static unsigned from;
    if (gGsFrame - from >= 60) {
        unsigned f = gGsFrame - from ? gGsFrame - from : 1;
        fprintf(stderr, "stg_native per frame: %.1f ms; meshes fast %u, near-cut %u, culled %u, old way %u; triangles cut %u; "
                        "materials refused %u\n", (double)sNs / 1e6 / f, sN[0] / f, sN[1] / f, sN[2] / f, sN[3] / f, sN[4] / f, sN[5] / f);
        memset(sN, 0, sizeof(sN));
        sNs = 0;
        from = gGsFrame;
    }
}
#endif

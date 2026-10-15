/*
 * Checks port/src/vu1_native.c against the VU1 interpreter (port/src/gs/gs_vu1.c) on real chains from the game's
 * files: every chain of a part goes through both, the interpreter running the original microcode (the listings'
 * bytes, assets/cod/1BF6B0.textbin.bin) with its GIF output read as the GS reads it, and the native code's records;
 * the two lists of triangles are compared in order: same count, corners within 1/16 pixel, depth within 1 step,
 * the same colours, s t q within 1e-5 relative, the same context, PRIM bits and TEX0.
 *
 *   gcc -m32 -O2 -msse2 -mfpmath=sse -ffp-contract=off -Iinclude -Iport/include -Iport/src/gs port/tests/work/vu1_check.c \
 *       port/src/vu1_native.c port/src/gs/gs_vu1.c -lm -o /tmp/vu1_check
 *   /tmp/vu1_check 4 gamedata/pzs3us1/00424.bin 1          (program, file, part of its offset table)
 *   /tmp/vu1_check 0 gamedata/pzs3us1/02877.bin 3          (a fighter's model parts; 1: their fade pass)
 *   VU1_PRMODE=0x48 /tmp/vu1_check 2 gamedata/pzs3us1/02877.bin 3   (2: program 2a, 3: 2b, the same chains; with
 *                                                                   PRMODECONT 0 and that PRMODE, as the shadows)
 *
 * Constants: cameras made here (inside the mesh's bounds looking along each axis, and from outside seeing all of
 * it), so that triangles cross every plane of the clip volume; for the fighters two bones a little apart, a light,
 * colours above 0x80. What it does not check: the state a record is drawn
 * with (gRndCtx, followed from the env packets) and the textures (only that TEX0 is the one the GS would have had).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/rnd.h"
#include "vu1_native.h"

/* ---- what gs_vu1.c needs of the GS front end ---- */
typedef struct GsVertex {
    float x, y;
    uint32_t z;
    float s, t, q;
    int u, v;
    uint8_t r, g, b, a;
} GsVertex;
typedef struct GsState {
    uint64_t prim, primRaw, rgbaq, st, uv, texa, prmodecont, prmode;
    uint64_t tex0[2], tex1[2], clamp[2], xyoffset[2], scissor[2], alpha[2], test[2], frame[2], zbuf[2], fba[2];
    uint64_t bitbltbuf, trxpos, trxreg, trxdir;
    float q;
    GsVertex vtx[3];
    int vcount, strip, transfer;
    uint32_t tx, ty, tw, th, tpos;
} GsState;
GsState gGs;
unsigned gGsFrame;
int gPortResim, gsSkipFrame;
const char *Port_GetEnv(const char *name) { (void)name; return NULL; }
int GsGpu_Enabled(void) { return 0; } /* the interpreter, never the shader paths */
void GsGpu_DrawVu0(int a, int b, const float *c, uint32_t d, const float *e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void GsGpu_DrawVu4(int b, const float *c, uint32_t d, const float *e) { (void)b; (void)c; (void)d; (void)e; }
void GsGpu_DrawVu6(int b, const float *c, uint32_t d, const float *e) { (void)b; (void)c; (void)d; (void)e; }
void Gs_RegWrite(uint32_t addr, uint64_t d) { (void)addr; (void)d; }
void Gs_Gif(const uint8_t *p, uint32_t qwc);
void GsVu1_Program(uint32_t addr, const uint32_t *words, uint32_t count);
void GsVu1_Unpack(uint32_t cmd, uint32_t num, uint32_t imm, const uint32_t *data);
void GsVu1_Call(int addr);
void GsVu1_SetBase(uint32_t v);
void GsVu1_SetOffset(uint32_t v);
void GsVu1_SetCycle(uint32_t cl, uint32_t wl);

/* ---- triangles, from either side ---- */
typedef struct Tri {
    int ctx;
    uint32_t bits; /* abe | gouraud << 1 | tme << 2 */
    uint64_t tex0;
    RndVtx v[3];
} Tri;
typedef struct List {
    Tri *t;
    size_t n, cap;
} List;
static List sRef, sNat;
static size_t sFanTris; /* of the microcode's: from the clipper */
static size_t sAdc;     /* its triangles left out (ADC) */
static size_t sBatches; /* MSCNTs run */

static void add(List *l, const Tri *t) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->t = realloc(l->t, l->cap * sizeof(Tri));
    }
    l->t[l->n++] = *t;
}

#define XYOFF_X 0x7000
#define XYOFF_Y 0x7200
#define STAGE_TEX0 0x0006580400002A00ull /* what StgModel_SetTexture binds (a stand-in: only compared) */

/* ---- the native side's renderer ---- */
uint32_t gRndCtxBad[2];
int Port_TakesRecords(void) { return 1; }
#define BOUND_TEX0_2 0x0006580400003D40ull /* the TEX0_2 bound before program 2 (a stand-in) */
#define SHADOW_TEX0 (0x2A00ull | 4ull << 14 | 0xC402ull << 19) /* ObjGs_AddShadowTexEnv(0x2A00, 4) */
static uint64_t sTex0Ctx0 = STAGE_TEX0;
uint64_t Rnd_Tex0(int ctx) { return ctx == 0 ? sTex0Ctx0 : BOUND_TEX0_2; }
static unsigned log2_of(int v) { unsigned k = 0; while ((1 << k) < v) { k++; } return k; }
uint32_t Rnd_TargetTex(uint32_t fbp, int w, int h, int psm) { /* (as port/src/rnd_fx.c) */
    return (fbp & 0x1FF) | ((uint32_t)psm & 0x3F) << 9 | RND_TEX_TARGET | log2_of(w) << 16 | log2_of(h) << 20;
}
static uint32_t sWantTex; /* the handle a textured record must have (program 6: the shadow page), 0: none */
static size_t sBadTex;
static uint32_t sPrmode; /* PRMODE with PRMODECONT 0 (VU1_PRMODE: the shadow passes' 0x48), 0: PRMODECONT 1 */
uint32_t Rnd_Prim(uint32_t prim) { return sPrmode ? (prim & 7) | (sPrmode & 0x7F8) : prim & 0x7FF; }
void Rnd_Vtx(RndVtx *o, int ctx, int32_t x, int32_t y, uint32_t z, float s, float t, float q, uint32_t rgba) {
    (void)ctx;
    o->x = (float)((x & 0xFFFF) - XYOFF_X) / 16.0f;
    o->y = (float)((y & 0xFFFF) - XYOFF_Y) / 16.0f;
    o->z = z;
    o->s = s;
    o->t = t;
    o->q = q;
    o->rgba = rgba;
}
int Rnd_Draw(RndPoly *p, int ctx, int abe, int gouraud) {
    Tri t;
    int k;
    if (p->tex != (p->gsTex0 != 0 ? sWantTex : 0)) {
        sBadTex++;
    }
    for (k = 0; k + 3 <= (int)p->n; k++) {
        t.ctx = ctx;
        t.bits = (uint32_t)abe | (uint32_t)gouraud << 1 | (p->gsTex0 != 0) << 2;
        t.tex0 = p->gsTex0;
        t.v[0] = p->v[k];
        t.v[1] = p->v[k + 1];
        t.v[2] = p->v[k + 2];
        add(&sNat, &t);
    }
    return 1;
}

/* ---- the GS, as far as these packets need it (as port/src/gs/gs_core.c reads them) ---- */
static struct {
    uint32_t prim;
    uint64_t tex0[2];
    uint32_t rgba;
    float s, t, q;
    RndVtx v[3];
    int n;
} gs;

static void gs_vertex(uint32_t x, uint32_t y, uint32_t z, int kick) {
    int type = gs.prim & 7;
    RndVtx v;
    Tri t;
    v.x = (float)((int)(x & 0xFFFF) - XYOFF_X) / 16.0f;
    v.y = (float)((int)(y & 0xFFFF) - XYOFF_Y) / 16.0f;
    v.z = z;
    v.s = gs.s;
    v.t = gs.t;
    v.q = gs.q;
    v.rgba = gs.rgba;
    if (gs.n < 3) {
        gs.v[gs.n++] = v;
    } else {
        gs.v[0] = gs.v[1];
        gs.v[1] = gs.v[2];
        gs.v[2] = v;
    }
    if (gs.n == 3 && kick && (type == 4 || type == 5)) {
        t.ctx = (gs.prim >> 9) & 1;
        t.bits = ((gs.prim >> 6) & 1) | ((gs.prim >> 3) & 1) << 1 | ((gs.prim >> 4) & 1) << 2;
        t.tex0 = (gs.prim >> 4) & 1 ? gs.tex0[t.ctx] : 0;
        memcpy(t.v, gs.v, sizeof(t.v));
        add(&sRef, &t);
        sFanTris += type == 5;
    }
    sAdc += gs.n == 3 && !kick;
    if (gs.n == 3 && type == 5) {
        gs.v[1] = gs.v[2];
        gs.n = 2;
    }
}

void Gs_Gif(const uint8_t *p, uint32_t qwc) {
    const uint64_t *q = (const uint64_t *)p;
    while (qwc != 0) {
        uint64_t tag = q[0], regs = q[1];
        uint32_t nloop = tag & 0x7FFF, flg = (tag >> 58) & 3, nreg = (tag >> 60) & 15, i, j;
        nreg = nreg ? nreg : 16;
        q += 2;
        qwc--;
        if (flg != 0) {
            fprintf(stderr, "vu1_check: a GIF packet not packed\n");
            exit(2);
        }
        if (nloop != 0 && ((tag >> 46) & 1)) {
            gs.prim = Rnd_Prim((uint32_t)(tag >> 47) & 0x7FF);
            gs.n = 0;
        }
        for (i = 0; i < nloop && qwc != 0; i++) {
            for (j = 0; j < nreg && qwc != 0; j++) {
                uint64_t lo = q[0], hi = q[1];
                switch ((regs >> (4 * j)) & 15) {
                case 0x1: gs.rgba = (uint32_t)(lo & 0xFF) | (uint32_t)((lo >> 32) & 0xFF) << 8 | (uint32_t)(hi & 0xFF) << 16 | (uint32_t)((hi >> 32) & 0xFF) << 24; break;
                case 0x2: memcpy(&gs.s, &lo, 4); memcpy(&gs.t, (const uint8_t *)&lo + 4, 4); memcpy(&gs.q, &hi, 4); break;
                case 0x4: gs_vertex(lo & 0xFFFF, (lo >> 32) & 0xFFFF, (uint32_t)((hi >> 4) & 0xFFFFFF), !((hi >> 47) & 1)); break;
                case 0xE:
                    if ((hi & 0xFF) == 6 || (hi & 0xFF) == 7) { gs.tex0[(hi & 0xFF) - 6] = lo; }
                    else if ((hi & 0xFF) == 0) { gs.prim = Rnd_Prim((uint32_t)lo & 0x7FF); gs.n = 0; }
                    break;
                case 0x6: case 0x7: gs.tex0[((regs >> (4 * j)) & 15) - 6] = lo; break; /* TEX0_1, TEX0_2 packed */
                case 0xF: break;
                default: fprintf(stderr, "vu1_check: GIF register %u\n", (unsigned)((regs >> (4 * j)) & 15)); exit(2);
                }
                q += 2;
                qwc--;
            }
        }
        if ((tag >> 15) & 1) {
            break;
        }
    }
}

/* ---- the reference run: the microcode on the chain, as VIF1 would feed it ---- */
static uint8_t *sBlob;
static size_t sBlobSize;

static void load_program(uint32_t at, uint32_t size) { /* the MPG codes of a program's packet in .vutext */
    const uint32_t *w = (const uint32_t *)(sBlob + at), *end = (const uint32_t *)(sBlob + at + size);
    while (w < end) {
        uint32_t code = *w++, cmd = (code >> 24) & 0x7F;
        if (cmd == 0x4A) {
            uint32_t num = (code >> 16) & 0xFF;
            num = num ? num : 256;
            GsVu1_Program(code & 0xFFFF, w, num * 2);
            w += num * 2;
        } else if (cmd != 0) {
            fprintf(stderr, "vu1_check: VIF code %08X in a program packet\n", code);
            exit(2);
        }
    }
}

static void run_chain(const uint32_t *t) {
    const uint32_t *w = t + 2, *end = t + 4 + (t[0] & 0xFFFF) * 4;
    while (w < end) {
        uint32_t code = *w++, cmd = (code >> 24) & 0x7F;
        if (cmd == 0x6C) {
            uint32_t num = (code >> 16) & 0xFF;
            num = num ? num : 256;
            GsVu1_Unpack(cmd, num, code & 0xFFFF, w);
            w += num * 4;
        } else if (cmd == 0x17) {
            GsVu1_Call(-1);
            sBatches++;
        } else if (cmd == 0x01) {
            GsVu1_SetCycle(code & 0xFF, (code >> 8) & 0xFF);
        }
    }
}

/* ---- chains of a part ---- */
static int is_chain(const uint32_t *t, const uint32_t *limit, int hdrFirst) {
    const uint32_t *w = t + 2, *end = t + 4 + (t[0] & 0xFFFF) * 4;
    int first = 1, mscnt = 0;
    if (((t[0] >> 28) & 7) != 6 || t[1] != 0 || (t[0] & 0xFFFF) == 0 || end > limit) {
        return 0;
    }
    while (w < end) {
        uint32_t code = *w++, cmd = (code >> 24) & 0x7F;
        if (cmd == 0x6C) {
            uint32_t num = (code >> 16) & 0xFF;
            num = num ? num : 256;
            if (first && ((code & 0x3FF) != 0 || (hdrFirst == 1 ? num != 1 : num < (uint32_t)hdrFirst))) {
                return 0;
            }
            first = 0;
            w += num * 4;
            if (w > end) {
                return 0;
            }
        } else if (cmd == 0x17) {
            mscnt++;
        } else if (cmd != 0 && cmd != 0x01 && cmd != 0x10) {
            return 0;
        }
    }
    return mscnt != 0;
}

static uint8_t *read_part(const char *path, int part, size_t *size) {
    FILE *f = fopen(path, "rb");
    uint8_t *b, *out;
    size_t n;
    uint32_t count, o, end, k;
    if (f == NULL) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(n);
    if (fread(b, 1, n, f) != n) { exit(2); }
    fclose(f);
    memcpy(&count, b, 4);
    if (part < 1 || (uint32_t)part > count) { fprintf(stderr, "no part %d\n", part); exit(2); }
    memcpy(&o, b + 4 + (part - 1) * 4, 4);
    o &= ~3u;
    end = (uint32_t)n;
    for (k = 0; k < count; k++) {
        uint32_t q;
        memcpy(&q, b + 4 + k * 4, 4);
        q &= ~3u;
        if (q > o && q < end) { end = q; }
    }
    out = aligned_alloc(16, ((end - o) + 15) & ~15u);
    memcpy(out, b + o, end - o);
    *size = end - o;
    free(b);
    return out;
}

/* ---- matrices (four columns: p' = x c0 + y c1 + z c2 + c3) ---- */
static void mul(float *o, const double *a, const double *b) { /* o = a * b */
    int i, j, k;
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            double s = 0;
            for (k = 0; k < 4; k++) { s += a[k * 4 + i] * b[j * 4 + k]; }
            o[j * 4 + i] = (float)s;
        }
    }
}

/* screen, clip, clip -> screen for a camera at eye looking along fwd (up given), near / far planes */
static void camera(float *out, const double *eye, const double *fwd, const double *up, double n, double f) {
    double r[3], u[3], view[16], proj[16], scr[16], clip[16];
    int i;
    r[0] = up[1] * fwd[2] - up[2] * fwd[1];
    r[1] = up[2] * fwd[0] - up[0] * fwd[2];
    r[2] = up[0] * fwd[1] - up[1] * fwd[0];
    u[0] = fwd[1] * r[2] - fwd[2] * r[1];
    u[1] = fwd[2] * r[0] - fwd[0] * r[2];
    u[2] = fwd[0] * r[1] - fwd[1] * r[0];
    memset(view, 0, sizeof(view));
    for (i = 0; i < 3; i++) {
        view[i * 4 + 0] = r[i];
        view[i * 4 + 1] = u[i];
        view[i * 4 + 2] = fwd[i];
    }
    view[12] = -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]);
    view[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    view[14] = -(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2]);
    view[15] = 1;
    memset(proj, 0, sizeof(proj)); /* x, y, z in -w..w; w = the distance ahead */
    proj[0] = 1.6;
    proj[5] = 1.6 * 512.0 / 448.0;
    proj[10] = (f + n) / (f - n);
    proj[11] = 1;
    proj[14] = -2 * f * n / (f - n);
    memset(scr, 0, sizeof(scr)); /* clip -> GS screen: 2048 +- 256 / 224, depth 16777000 (near) .. 256 (far) */
    scr[0] = 256;
    scr[5] = -224;
    scr[10] = (256.0 - 16777000.0) / 2;
    scr[12] = 2048;
    scr[13] = 2048;
    scr[14] = (256.0 + 16777000.0) / 2;
    scr[15] = 1;
    {
        double c2[16];
        int j, k;
        for (j = 0; j < 4; j++) {
            for (i = 0; i < 4; i++) {
                double s = 0;
                for (k = 0; k < 4; k++) { s += proj[k * 4 + i] * view[j * 4 + k]; }
                c2[j * 4 + i] = s;
            }
        }
        memcpy(clip, c2, sizeof(clip));
    }
    mul(out, scr, clip);
    for (i = 0; i < 16; i++) { out[16 + i] = (float)clip[i]; out[32 + i] = (float)scr[i]; }
}

/* ---- comparison ---- */
static double sWorstXY, sWorstZ, sWorstSTQ;
static size_t sBad;

static double rel(float a, float b) {
    double m = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return m < 1e-30 ? 0 : fabs((double)a - b) / m;
}

static void compare(const char *what) {
    size_t i, n = sRef.n < sNat.n ? sRef.n : sNat.n;
    if (sRef.n != sNat.n) {
        if (sBad++ < 10) { printf("%s: %zu triangles from the microcode, %zu records\n", what, sRef.n, sNat.n); }
    }
    for (i = 0; i < n; i++) {
        const Tri *a = &sRef.t[i], *b = &sNat.t[i];
        int k, bad = a->ctx != b->ctx || a->bits != b->bits || a->tex0 != b->tex0;
        for (k = 0; k < 3; k++) {
            const RndVtx *p = &a->v[k], *q = &b->v[k];
            double dxy = fabs(p->x - q->x) > fabs(p->y - q->y) ? fabs(p->x - q->x) : fabs(p->y - q->y);
            double dz = fabs((double)p->z - (double)q->z), d = rel(p->s, q->s);
            if (rel(p->t, q->t) > d) { d = rel(p->t, q->t); }
            if (rel(p->q, q->q) > d) { d = rel(p->q, q->q); }
            if (dxy > sWorstXY) { sWorstXY = dxy; }
            if (dz > sWorstZ) { sWorstZ = dz; }
            if (d > sWorstSTQ) { sWorstSTQ = d; }
            if (dxy > 1.0 / 16 || dz > 1 || d > 1e-5 || p->rgba != q->rgba) { bad = 1; }
        }
        if (bad && sBad++ < 10) {
            printf("%s: triangle %zu differs (ctx %d %d, bits %X %X, TEX0 %016llX %016llX)\n", what, i, a->ctx, b->ctx, (unsigned)a->bits, (unsigned)b->bits, (unsigned long long)a->tex0, (unsigned long long)b->tex0);
            for (k = 0; k < 3; k++) {
                const RndVtx *p = &a->v[k], *q = &b->v[k];
                printf("   ref %9.4f %9.4f %8u %10.6g %10.6g %10.6g %08X | nat %9.4f %9.4f %8u %10.6g %10.6g %10.6g %08X\n", p->x, p->y, p->z,
                       p->s, p->t, p->q, p->rgba, q->x, q->y, q->z, q->s, q->t, q->q, q->rgba);
            }
        }
    }
}

/* the positions of a chain's corners (three quadwords each, after a header of h) into the bounds */
static void bounds(const uint32_t *t, uint32_t h, float *lo, float *hi) {
    const uint32_t *w = t + 2, *end = t + 4 + (t[0] & 0xFFFF) * 4;
    while (w < end) {
        uint32_t code = *w++, num = (code >> 16) & 0xFF, a, k, i;
        if (((code >> 24) & 0x7F) != 0x6C) { continue; }
        num = num ? num : 256;
        a = code & 0x3FF;
        for (k = a < h ? h - a : (a - h) % 3 == 0 ? 0 : num; k + 3 <= num; k += 3) {
            for (i = 0; i < 3; i++) {
                float v;
                memcpy(&v, w + k * 4 + i, 4);
                if (v < lo[i]) { lo[i] = v; }
                if (v > hi[i]) { hi[i] = v; }
            }
        }
        w += num * 4;
    }
}

/* a column-major rotation about an axis (x, y, z unit) by angle, with a translation */
static void rot_mtx(float *m, double x, double y, double z, double a, double tx, double ty, double tz) {
    double c = cos(a), sn = sin(a), t = 1 - c;
    float r[16] = {(float)(t * x * x + c), (float)(t * x * y + sn * z), (float)(t * x * z - sn * y), 0,
                   (float)(t * x * y - sn * z), (float)(t * y * y + c), (float)(t * y * z + sn * x), 0,
                   (float)(t * x * z + sn * y), (float)(t * y * z - sn * x), (float)(t * z * z + c), 0,
                   (float)tx, (float)ty, (float)tz, 1};
    memcpy(m, r, sizeof(r));
}

/* ---- program 6: the chain ObjShadow_BuildPacket makes (src/sys/gfxm_e_b.c, its C attempt) from triangles ---- */
typedef struct ShTri {
    float v[3][4], normal[4];
} ShTri;
#define SH_MAX 256 /* triangles taken of a chain */

static size_t build_p6(uint32_t *pkt, int count, const ShTri *tris, const float *color) {
    uint32_t *p, *tag;
    float *f;
    int i = 0, j, k;
    size_t n;

    pkt[0] = 0x60000000; pkt[1] = 0; pkt[2] = 0x6C018000; pkt[3] = 0x8000;
    pkt[4] = 0x302EC000; pkt[5] = 0x412; pkt[6] = 0; pkt[7] = 0x17000000;
    p = pkt + 8;
    do {
        *p++ = 0x6C038000; *p++ = 0x8001; *p++ = 0x10000000; *p++ = 6;
        *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;
        *p++ = 0;
        tag = p;
        *p++ = 0; *p++ = 0x302E4000; *p++ = 0x412; *p++ = 0;
        *p++ = 0;
        f = (float *)p;
        j = 0;
        do {
            for (k = 0; k < 3; k++) {
                *f++ = tris[i].v[k][0] + tris[i].normal[0] * 0.02f;
                *f++ = tris[i].v[k][1] + tris[i].normal[1] * 0.02f;
                *f++ = tris[i].v[k][2] + tris[i].normal[2] * 0.02f;
                p = (uint32_t *)f;
                *p++ = k < 2 && j != 0;
                f = (float *)p;
                f[0] = tris[i].normal[0]; f[1] = tris[i].normal[1]; f[2] = tris[i].normal[2]; f[3] = 1.0f;
                f[4] = color[0]; f[5] = color[1]; f[6] = color[2]; f[7] = color[3];
                f[8] = 0.0f; f[9] = 0.0f; f[10] = 1.0f; f[11] = 0.0f;
                f += 12;
            }
            i++;
            j++;
        } while (i < count && j < 18);
        p = (uint32_t *)f;
        tag[0] = (uint32_t)(j * 3) | 0x8000;
        tag[4] = ((uint32_t)(j * 3) << 18) | 0x6C008003;
        *p++ = 0x17000000;
    } while (i < count);
    while ((p - pkt) % 4 != 0) {
        *p++ = 0;
    }
    *p++ = 0x70000000; *p++ = 0; *p++ = 0; *p++ = 0;
    n = (size_t)(p - pkt);
    pkt[0] = (uint32_t)(n / 4) + 0x5FFFFFFE;
    return n;
}

/* the triangles of a chain's strips (vertices three quadwords each behind a header of h), with their normals */
static int chain_tris(const uint32_t *t, uint32_t h, ShTri *out) {
    const uint32_t *w = t + 2, *end = t + 4 + (t[0] & 0xFFFF) * 4;
    int n = 0;
    while (w < end && n < SH_MAX) {
        uint32_t code = *w++, num = (code >> 16) & 0xFF, k, i;
        if (((code >> 24) & 0x7F) != 0x6C) { continue; }
        num = num ? num : 256;
        if ((code & 0x3FF) == h) {
            for (k = 2; k < num / 3 && n < SH_MAX; k++) {
                double a[3], b[3], c[3], len;
                for (i = 0; i < 3; i++) {
                    memcpy(out[n].v[i], w + (k - 2 + i) * 12, 16);
                    out[n].v[i][3] = 1.0f;
                }
                for (i = 0; i < 3; i++) { a[i] = out[n].v[1][i] - out[n].v[0][i]; b[i] = out[n].v[2][i] - out[n].v[0][i]; }
                c[0] = a[1] * b[2] - a[2] * b[1]; c[1] = a[2] * b[0] - a[0] * b[2]; c[2] = a[0] * b[1] - a[1] * b[0];
                len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                if (len < 1e-12) { c[0] = 0; c[1] = 1; c[2] = 0; len = 1; }
                for (i = 0; i < 3; i++) { out[n].normal[i] = (float)(c[i] / len); }
                out[n].normal[3] = 1.0f;
                n++;
            }
        }
        w += num * 4;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *blobPath = getenv("VU1_BLOB") ? getenv("VU1_BLOB") : "assets/cod/1BF6B0.textbin.bin";
    int prog, part, cam;
    uint32_t hdrSize;
    uint8_t *b;
    size_t size, off, chains = 0, tris = 0, recs = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    FILE *f;

    if (argc < 4) {
        fprintf(stderr, "usage: vu1_check <program 0, 1, 2 (2a), 3 (2b) or 4> <file> <part>\n");
        return 2;
    }
    prog = atoi(argv[1]);
    part = atoi(argv[3]);
    if (prog < 0 || prog == 5 || prog > 8) {
        fprintf(stderr, "program %d: not here\n", prog);
        return 2;
    }
    if (getenv("VU1_PRMODE") != NULL) {
        sPrmode = (uint32_t)strtoul(getenv("VU1_PRMODE"), NULL, 0);
    }
    hdrSize = prog == 4 || (prog == 6 && part != 3) ? 1 : prog == 7 || prog == 8 ? 3 : 5;
    f = fopen(blobPath, "rb");
    if (f == NULL) { perror(blobPath); return 2; }
    fseek(f, 0, SEEK_END);
    sBlobSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    sBlob = malloc(sBlobSize);
    if (fread(sBlob, 1, sBlobSize, f) != sBlobSize) { return 2; }
    fclose(f);
    b = read_part(argv[2], part, &size);
    if (prog == 7 || prog == 8) { /* only a stage's (part 1 with the stage's chains, those with the object header) */
        size_t n4 = 0, size1 = size;
        uint8_t *b1 = part == 1 ? b : read_part(argv[2], 1, &size1);
        for (off = 0; off + 16 <= size1 && n4 == 0; off += 16) {
            n4 += is_chain((const uint32_t *)(b1 + off), (const uint32_t *)(b1 + size1), 1);
        }
        if (n4 == 0) {
            printf("program %d: 0 chains (not a stage)\n", prog);
            return 0;
        }
    }
    for (off = 0; off + 16 <= size; off += 16) {
        const uint32_t *t = (const uint32_t *)(b + off);
        if (is_chain(t, (const uint32_t *)(b + size), (int)hdrSize)) {
            bounds(t, hdrSize == 5 ? 5 : 3, lo, hi);
            chains++;
            off += (t[0] & 0xFFFF) * 16;
        }
    }
    printf("program %d: %zu chains, positions %.1f..%.1f %.1f..%.1f %.1f..%.1f\n", prog, chains, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    for (cam = 0; cam < 7; cam++) {
        static const double dirs[7][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0.6, -0.3, 0.74}, {-0.5, 0.2, -0.84}, {0, 0, 1}};
        double eye[3], fwd[3], up[3] = {0, 1, 0}, span = 0, len = 0;
        float consts[48], k[0x22 * 4];
        int i;
        for (i = 0; i < 3; i++) {
            eye[i] = (lo[i] + hi[i]) / 2;
            span = hi[i] - lo[i] > span ? hi[i] - lo[i] : span;
            len += dirs[cam][i] * dirs[cam][i];
        }
        for (i = 0; i < 3; i++) {
            fwd[i] = dirs[cam][i] / sqrt(len);
        }
        if (cam == 6) { /* from outside, all of it in view */
            eye[2] -= span * 1.5;
        } else if (hdrSize == 5) { /* a model: from just outside it, so that some of it is out of view */
            for (i = 0; i < 3; i++) { eye[i] -= fwd[i] * span * 0.6; }
        }
        camera(consts, eye, fwd, up, span * 0.002, span * 4);
        /* programs 0, 1: ObjDrawPkt from mtxA (bones A, B moving the model a little apart, so the weights count) */
        memset(k, 0, sizeof(k));
        rot_mtx(k, 0, 1, 0, 0.1 * cam, span * 0.01, 0, 0);
        rot_mtx(k + 16, 0.6, 0.8, 0, -0.2, 0, span * 0.02, 0);
        k[32] = (float)(span * 0.003); k[33] = 0; k[34] = 0; k[35] = 1;
        k[36] = 0; k[37] = (float)(-span * 0.002); k[38] = 0; k[39] = 1;
        rot_mtx(k + 40, 0.36, 0.48, 0.8, 0.7 + cam, 0, 0, 0);
        k[40] = -0.3f; k[44] = 0.5f; k[48] = -0.81f; /* x of the light's columns: -L */
        memcpy(k + 56, consts, 32 * sizeof(float)); /* screen, clip */
        k[88] = 128; k[89] = 96; k[90] = 200.5f; k[91] = 128 - cam;
        k[92] = 128; k[93] = 128; k[94] = 128; k[95] = 128;
        k[104] = 0; k[105] = 0; /* constant 26: A+D of TEX0_2 (program 1) */
        {
            uint32_t ad[4] = {0x12345678u, 0x00000ABCu, 7, 0};
            memcpy(k + 104, ad, 16);
        }
        for (off = 0; off + 16 <= size; off += 16) {
            const uint32_t *t = (const uint32_t *)(b + off);
            if (!is_chain(t, (const uint32_t *)(b + size), (int)hdrSize)) {
                continue;
            }
            sRef.n = sNat.n = 0;
            if (prog == 6) {
                /* triangles of the chain (a stage's, or a model's in part 3) under a shadow: ObjShadow_BuildPacket,
                   Vu1Pkt_LoadProg6 (17 quadwords: screen, clip, clip -> screen, a shadow camera from above, the
                   scale), Vu1Pkt_CallProg6 and the chain */
                static ShTri tris[SH_MAX];
                static uint32_t pkt[8 + (SH_MAX / 18 + 1) * 16 + SH_MAX * 48 + 16] __attribute__((aligned(16)));
                static const float color[4] = {120.0f, 110.5f, 99.0f, 77.0f};
                float k6[17 * 4], sp = (float)(2.0 / (span > 0 ? span : 1));
                int nt = chain_tris(t, 3 + (hdrSize == 5) * 2, tris);
                if (nt == 0) {
                    off += (t[0] & 0xFFFF) * 16;
                    continue;
                }
                build_p6(pkt, nt, tris, color);
                memset(k6, 0, sizeof(k6));
                memcpy(k6, consts, 48 * sizeof(float));
                k6[48] = sp; k6[50] = 0.001f; /* x's column: c.x = x * 2 / span, c.z = y / 1000 */
                k6[53] = 0.1f * sp; k6[54] = 0.001f; /* y's column */
                k6[57] = sp; /* z's column: c.y = z * 2 / span */
                k6[60] = -(lo[0] + hi[0]) / 2 * sp; k6[61] = -(lo[2] + hi[2]) / 2 * sp; k6[63] = 1.0f;
                k6[64] = 0.9f + 0.02f * cam;
                load_program(0x2C1F00 - 0x2BF6B0, 0x2C2D30 - 0x2C1F00);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 0x11, 0, (const uint32_t *)k6);
                GsVu1_SetBase(0x92);
                GsVu1_SetOffset(0x1B2);
                GsVu1_Call(0);
                GsVu1_SetBase(0x92);
                GsVu1_SetOffset(0x1B2);
                gs.tex0[0] = SHADOW_TEX0;
                sTex0Ctx0 = SHADOW_TEX0;
                sWantTex = Rnd_TargetTex(0x150, 256, 256, 1) | RND_TEX_RGB;
                run_chain(pkt);
                Vu1Native_SetProg6(k6);
                if (!Vu1Native_Prog6(pkt)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                }
            } else if (prog == 8) {
                /* Vu1Pkt_LoadProg8 (0x12 quadwords, screen and clip at 10..17 filled by StgModel_DrawAnims), then
                   per node Vu1Pkt_CallProg8 (10: world A, parent B, ofsA, ofsB) and the node's chain */
                static float k8[0x12 * 4] __attribute__((aligned(16)));
                memset(k8, 0, sizeof(k8));
                rot_mtx(k8, 0.36, 0.48, 0.8, 0.3 * cam - 0.5, span * 0.01, -span * 0.02, span * 0.005);
                rot_mtx(k8 + 16, 0.6, 0.8, 0, -0.2 + 0.1 * cam, 0, span * 0.03, 0);
                k8[32] = (float)(span * 0.003); k8[33] = 0; k8[34] = (float)(span * 0.001); k8[35] = 1;
                k8[36] = 0; k8[37] = (float)(-span * 0.002); k8[38] = 0; k8[39] = 1;
                memcpy(k8 + 40, consts, 32 * sizeof(float));
                load_program(0x2C3080 - 0x2BF6B0, 0x300);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 0x12, 0, (const uint32_t *)k8);
                GsVu1_SetBase(0x12);
                GsVu1_SetOffset(0x1F7);
                GsVu1_Unpack(0x6C, 0xA, 0, (const uint32_t *)k8);
                GsVu1_Call(0);
                GsVu1_SetBase(0x12);
                GsVu1_SetOffset(0x1F7);
                run_chain(t);
                Vu1Native_SetProg8(k8);
                if (!Vu1Native_Prog8(t, k8)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                }
            } else if (prog == 7) {
                /* Vu1Pkt_LoadProg7, then Vu1Pkt_CallProg7 (13 quadwords: a mesh matrix, screen, clip, the alpha) and
                   the chain; the chains of the stage without the object header (the debris') */
                static float k7[13 * 4] __attribute__((aligned(16)));
                rot_mtx(k7, 0.36, 0.48, 0.8, 0.3 * cam - 0.5, span * 0.01, -span * 0.02, span * 0.005);
                memcpy(k7 + 16, consts, 32 * sizeof(float));
                k7[48] = 1.0f; k7[49] = 2.0f; k7[50] = 3.0f;
                k7[51] = 40.0f + 31.7f * cam;
                load_program(0x2C2D30 - 0x2BF6B0, 0x2C3080 - 0x2C2D30);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 13, 0, (const uint32_t *)k7);
                GsVu1_Call(0);
                GsVu1_SetBase(0xD);
                GsVu1_SetOffset(0x1F9);
                gs.tex0[0] = STAGE_TEX0;
                run_chain(t);
                if (!Vu1Native_Prog7(t, k7)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                }
            } else if (prog == 4) {
                /* the reference: Vu1Pkt_LoadProg4, then Vu1Pkt_CallProg4 and the chain */
                load_program(0x2C04F0 - 0x2BF6B0, 0x2C1180 - 0x2C04F0);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 12, 0, (const uint32_t *)consts);
                GsVu1_SetBase(0x8D);
                GsVu1_SetOffset(0x1B4);
                GsVu1_Call(0);
                GsVu1_SetBase(0x8D);
                GsVu1_SetOffset(0x1B4);
                gs.tex0[0] = STAGE_TEX0;
                run_chain(t);
                Vu1Native_SetProg4(consts);
                if (!Vu1Native_Prog4(t)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                }
            } else if (prog == 2 || prog == 3) {
                /* Vu1Pkt_LoadProg2 (0x13 quadwords: bones, pivots, colour, then screen 11..14 and clip 15..18), then
                   Vu1Pkt_CallProg2 (0..10 again) and the chain */
                static float k2[0x13 * 4];
                memcpy(k2, k, 40 * sizeof(float));
                memcpy(k2 + 40, k + 88, 4 * sizeof(float)); /* the colour: program 0's layer 0 colour */
                memcpy(k2 + 44, consts, 32 * sizeof(float));
                load_program(prog == 2 ? 0x2BFEA0 - 0x2BF6B0 : 0x2C01C0 - 0x2BF6B0, prog == 2 ? 0x320 : 0x330);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 0x13, 0, (const uint32_t *)k2);
                GsVu1_SetBase(0x13);
                GsVu1_SetOffset(0x1F6);
                GsVu1_Unpack(0x6C, 0xB, 0, (const uint32_t *)k2);
                GsVu1_Call(0);
                GsVu1_SetBase(0x13);
                GsVu1_SetOffset(0x1F6);
                gs.tex0[1] = BOUND_TEX0_2;
                run_chain(t);
                Vu1Native_SetProg2(prog == 3, k2);
                if (!Vu1Native_Takes(2, t)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                } else {
                    Vu1Native_Prog2(t, k2);
                }
            } else {
                /* Vu1Pkt_LoadProg0 / 1, then Vu1Pkt_CallProg0 / 1 and the chain */
                load_program(prog == 0 ? 0 : 0x400, prog == 0 ? 0x400 : 0x3F0);
                GsVu1_SetCycle(1, 1);
                GsVu1_Unpack(0x6C, 0x22, 0, (const uint32_t *)k);
                GsVu1_Call(0);
                GsVu1_SetBase(0x22);
                GsVu1_SetOffset(0x1EF);
                run_chain(t);
                if (!Vu1Native_Takes(prog, t)) {
                    printf("chain at +0x%zX: not taken\n", off);
                    sBad++;
                } else {
                    (prog == 0 ? Vu1Native_Prog0 : Vu1Native_Prog1)(t, k);
                }
            }
            {
                char what[64];
                snprintf(what, sizeof(what), "camera %d chain +0x%zX", cam, off);
                compare(what);
            }
            tris += sRef.n;
            recs += sNat.n;
            off += (t[0] & 0xFFFF) * 16;
        }
    }
    if (sBadTex != 0) {
        printf("%zu records with another texture handle\n", sBadTex);
        sBad += sBadTex;
    }
    printf("%zu batches; %zu triangles from the microcode (%zu of them clipped; %zu left out), %zu from records; worst corner %.4f px, depth %.0f, s t q %.3g relative; %zu differences\n",
           sBatches, tris, sFanTris, sAdc, recs, sWorstXY, sWorstZ, sWorstSTQ, sBad);
    return sBad != 0;
}

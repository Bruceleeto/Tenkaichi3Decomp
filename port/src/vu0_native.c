/*
 * The vector / matrix library (the PS2's VU0 in macro mode: src/sys/vu0_a_c.c, vu0_b_c.c) as what each routine
 * computes, with the SH4's own vector instructions through sh4zam (fipr, fsrra, ftrv, fsca). The Dreamcast and
 * make dcsim only; the PC keeps the exact reference (src/port/vu0_a.c, vu0_b.c), the PS2's numbers bit for bit.
 * Each routine's comment in the reference says what the original does instruction by instruction.
 *
 * The numbers are the SH4's, not the PS2's: rounded, not cut toward zero; 1 / x and 1 / sqrt by fsrra; sine and
 * cosine by fsca. What the game can see besides the numbers is kept: which components are written (a "Vec3" routine
 * stores w as well, with what the original leaves there), a division by zero giving +/-FLT_MAX as the PS2 does (it
 * has no infinity or NaN), an output that is also an input, and the original's bugs that change results.
 *
 * The VU0's registers that outlive a routine are kept here: the current matrix (vf16..19), its stack in VU0 memory
 * (vi15), and the clip, screen and view matrices (vf20..31). The constants vf0..3 are literals. A matrix is four
 * rows of four floats, row i holding what the PS2 multiplies by v[i]: sh4zam's column order, loaded into XMTRX as is.
 *
 * The reference's objects are linked weak in these builds (Makefile.dc, Makefile's dcsim): a routine defined here
 * replaces the reference's.
 */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sh4zam/shz_sh4zam.h>

typedef struct V4 {
    float x, y, z, w;
} V4; /* the game's vectors (Vec4, EftVVec ...): 4-aligned here */

typedef struct I4 {
    int32_t x, y, z, w;
} I4;

typedef struct M4 {
    V4 r[4];
} M4;

typedef struct ClipVtx {
    V4 pos, uv, col;
} ClipVtx;

extern void Rand_SeedFloat(float seed);
extern void Rand_Init(void);

/* ------------------------------------------------------------------------------------------------------------------
 * State and helpers
 * ---------------------------------------------------------------------------------------------------------------- */

static M4 sCur __attribute__((aligned(8))) = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
static M4 sClip __attribute__((aligned(8)));
static M4 sScreen __attribute__((aligned(8)));
static M4 sView __attribute__((aligned(8)));
static V4 sStack[256]; /* VU0 data memory: 256 quadwords, the address wraps */
static int16_t sSp;     /* vi15, in quadwords */

static const M4 kIdentity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};

static inline V4 v4(float x, float y, float z, float w) {
    V4 v = {x, y, z, w};
    return v;
}

static inline shz_vec4_t to_shz(V4 v) { return shz_vec4_init(v.x, v.y, v.z, v.w); }
static inline V4 from_shz(shz_vec4_t v) { return v4(v.x, v.y, v.z, v.w); }

#ifndef BT3_DCSIM
/* The SH4's four approximate instructions (ftrv, fipr, fsrra, fsca) through sh4zam, and its compositions of them. */

/* XMTRX = m (a matrix of the game's: 4-aligned) / one of ours (8-aligned) */
static inline void xload(const M4 *m) { shz_xmtrx_load_unaligned_4x4((const float *)m); }
static inline void xload_own(const M4 *m) { shz_xmtrx_load_4x4((const shz_mat4x4_t *)m); }

/* XMTRX * v: ((r0 * v.x + r1 * v.y) + r2 * v.z) + r3 * v.w, four components */
static inline V4 xf(V4 v) { return from_shz(shz_xmtrx_transform_vec4(to_shz(v))); }

static inline float dot3(V4 a, V4 b) { return shz_vec3_dot(shz_vec3_init(a.x, a.y, a.z), shz_vec3_init(b.x, b.y, b.z)); }
static inline float dot4(V4 a, V4 b) { return shz_vec4_dot(to_shz(a), to_shz(b)); }
static inline float nat_invf(float x) { return shz_invf(x); }
static inline float nat_sqrtf(float x) { return shz_sqrtf(x); }
static inline shz_sincos_t sc(float angle) { return shz_sincosf(angle); }
static inline V4 nat_normalize3(V4 v) { shz_vec3_t n = shz_vec3_normalize_safe(shz_vec3_init(v.x, v.y, v.z)); return v4(n.x, n.y, n.z, 0.0f); }
#else
/*
 * make dcsim: the same four instructions as the Dreamcast's computes them, bit for bit (port/third_party/sh4_fpu_approx,
 * measured on the hardware), in sh4zam's compositions and with its registers' order (which operand a product recodes
 * changes its bits): the Dreamcast's numbers but for its fmac (gcc fuses a * b + c there, not here).
 */
#include "../third_party/sh4_fpu_approx/sh4_fpu_approx.h"

static uint32_t sXmtrx[16];

static inline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static inline void xload(const M4 *m) { memcpy(sXmtrx, m, sizeof(sXmtrx)); }
static inline void xload_own(const M4 *m) { memcpy(sXmtrx, m, sizeof(sXmtrx)); }

/* ftrv xmtrx, fv8 */
static inline V4 xf(V4 v) {
    uint32_t fv[4];

    memcpy(fv, &v, 16);
    sh4_ftrv(sXmtrx, fv, 0);
    memcpy(&v, fv, 16);
    return v;
}

/* fipr fv4, fv8 as shz_dot8f_sh4 loads them: fv4 = (y1, x1, w1, z1), fv8 = (y2, x2, w2, z2) */
static inline float fipr8(float x1, float y1, float z1, float w1, float x2, float y2, float z2, float w2) {
    uint32_t m[4] = {fbits(y1), fbits(x1), fbits(w1), fbits(z1)}, n[4] = {fbits(y2), fbits(x2), fbits(w2), fbits(z2)};

    return bitsf(sh4_fipr(m, n, 0));
}

static inline float dot3(V4 a, V4 b) { return fipr8(a.x, a.y, a.z, 0.0f, b.x, b.y, b.z, 0.0f); }
static inline float dot4(V4 a, V4 b) { return fipr8(a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w); }

static inline float fsrra(float x) { return bitsf(sh4_fsrra(fbits(x), 0)); }

/* shz_invf: fsrra(x x), negated for x < 0 */
static inline float nat_invf(float x) {
    float inv = fsrra(x * x);

    return x < 0.0f ? -inv : inv;
}

/* shz_sqrtf: fsrra(x) x */
static inline float nat_sqrtf(float x) { return x == 0.0f ? 0.0f : fsrra(x) * x; }

/* sinf / cosf under -ffast-math: fmul by 2^15 / pi, ftrc (toward zero, saturating) to fpul, fsca */
static inline shz_sincos_t sc(float angle) {
    float t = angle * 10430.37835f;
    uint32_t fpul, sb, cb;
    shz_sincos_t r;

    if (t != t || t <= -2147483648.0f) {
        fpul = 0x80000000u;
    } else if (t >= 2147483648.0f) {
        fpul = 0x7FFFFFFFu;
    } else {
        fpul = (uint32_t)(int32_t)t;
    }
    sh4_fsca(fpul, 0, &sb, &cb);
    r.sin = bitsf(sb);
    r.cos = bitsf(cb);
    return r;
}

/* shz_vec3_normalize_safe: the square by fipr fv4, fv4 = (y, x, 0, z), then x fsrra(it) */
static inline V4 nat_normalize3(V4 v) {
    uint32_t m[4] = {fbits(v.y), fbits(v.x), 0, fbits(v.z)};
    float mag = bitsf(sh4_fipr(m, m, 0)), k;

    if (!(mag > 0.0f)) {
        return v4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    k = fsrra(mag);
    return v4(v.x * k, v.y * k, v.z * k, 0.0f);
}
#endif

/* atan2: the game's own (as the PS2's Vec3_DirToEuler calls libm's). sh4zam's shz_atan2f is off by up to 0.01 rad,
 * and the game turns the camera's direction into angles and back each frame: the error grew until the camera was lost */
#ifdef __DREAMCAST__
static inline float nat_atan2f(float y, float x) { return atan2f(y, x); } /* (newlib's: the Dreamcast has no bridge) */
#else
float Port_atan2f(float, float);
static inline float nat_atan2f(float y, float x) { return Port_atan2f(y, x); }
#endif

/* a / b as the PS2 divides: a zero divisor gives +/-FLT_MAX with the sign of sign(a) xor sign(b) */
static inline float div_ps2(float a, float b) {
    if (b == 0.0f) {
        return (signbit(a) != signbit(b)) ? -FLT_MAX : FLT_MAX;
    }
    return a / b;
}

/* The PS2 has no infinity or NaN: a product too large is +/-FLT_MAX (and FLT_MAX * 0 is 0). The game's code on the
 * Dreamcast is IEEE: an infinity from here became a NaN there (0 * inf) and spread through the camera */
static inline float ps2f(float r) { return __builtin_fabsf(r) > FLT_MAX ? copysignf(FLT_MAX, r) : r; }
/* (|r| > FLT_MAX: isinf for every value, NaN too, as one float compare; isinf was libgcc's __unordsf2 on the SH4) */
static inline float pmul(float a, float b) { return ps2f(a * b); }

/* 1 / w (vdiv Q, vf0w, w): a zero or denormal w (the PS2 reads it as 0) gives +/-FLT_MAX; a w outside 2^-60..2^60,
 * where fsrra(w * w) under- or overflows, is divided */
static inline float recip(float w) {
    uint32_t u, e;

    memcpy(&u, &w, 4);
    e = (u >> 23) & 0xFF;
    if (e == 0) {
        return (u >> 31) ? -FLT_MAX : FLT_MAX;
    }
    if (e < 127 - 60 || e > 127 + 60) {
        return ps2f(1.0f / w);
    }
    return nat_invf(w);
}

/* vftoi: truncate toward zero, saturating */
static inline int32_t ftoi(float f) {
    if (f >= 2147483648.0f) {
        return INT32_MAX;
    }
    if (f <= -2147483648.0f) {
        return INT32_MIN;
    }
    if (f != f) {
        return 0;
    }
    return (int32_t)f;
}

/* rotates within each row: row[i]' = c row[i] - s row[j], row[j]' = s row[i] + c row[j] (i, j: component indices) */
static void rot_in_rows(M4 *dst, const M4 *src, int i, int j, float c, float s) {
    M4 t = *src;
    int k;

    for (k = 0; k < 4; k++) {
        float *r = &t.r[k].x;
        float a = r[i], b = r[j];

        r[i] = c * a - s * b;
        r[j] = s * a + c * b;
    }
    *dst = t;
}

/* combines two rows: row i' = c row i - s row j, row j' = s row i + c row j (four components) */
static void comb_rows(M4 *m, int i, int j, float c, float s) {
    V4 a = m->r[i], b = m->r[j];

    m->r[i] = v4(c * a.x - s * b.x, c * a.y - s * b.y, c * a.z - s * b.z, c * a.w - s * b.w);
    m->r[j] = v4(s * a.x + c * b.x, s * a.y + c * b.y, s * a.z + c * b.z, s * a.w + c * b.w);
}

/* a * b: row i of the result = a * (row i of b) */
static void mtx_mul(M4 *dst, const M4 *a, const M4 *b) {
    M4 t;

    xload(a);
    t.r[0] = xf(b->r[0]);
    t.r[1] = xf(b->r[1]);
    t.r[2] = xf(b->r[2]);
    t.r[3] = xf(b->r[3]);
    *dst = t;
}

/* the perspective step of every projection: p.xyz *= 1 / p.w, p.w kept; returns Q = 1 / p.w */
static inline float persp(V4 *p) {
    float q = recip(p->w);

    p->x = pmul(p->x, q);
    p->y = pmul(p->y, q);
    p->z = pmul(p->z, q);
    return q;
}

/* the on-screen test: 0 < x < 4096, 0 < y < 4096, w > 0 (x, y divided, w not) */
static inline int on_screen(V4 p) { return p.x > 0.0f && p.y > 0.0f && p.w > 0.0f && p.x < 4096.0f && p.y < 4096.0f; }

/* vftoi4.xy / vftoi0.zw */
static inline I4 to_gs(V4 p) {
    I4 o = {ftoi(p.x * 16.0f), ftoi(p.y * 16.0f), ftoi(p.z), ftoi(p.w)};
    return o;
}

/* (uv.x Q, uv.y Q, Q, w) */
static inline V4 stq(V4 uv, float q, float w) { return v4(pmul(uv.x, q), pmul(uv.y, q), q, w); }

static void normalize3(V4 *out, V4 v) { *out = nat_normalize3(v); }

static V4 cross3(V4 a, V4 b) {
    shz_vec3_t c = shz_vec3_cross(shz_vec3_init(a.x, a.y, a.z), shz_vec3_init(b.x, b.y, b.z));

    return v4(c.x, c.y, c.z, 0.0f);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x10FFD0 ObjSeam_TransformVtx: skins, lights and projects one seam vertex (src/port/vu0_a.c seam_c says what each
 * instruction does). The PS2 leaves vf1..16 and vf19 changed and its caller restores vf1..3: the current matrix's
 * row 0 (vf16) stays the vertex's position and row 3 (vf19) (4096, 4096, 0, 0) (one 64-bit move), and the game draws on with
 * that, so they are left so here too.
 * ---------------------------------------------------------------------------------------------------------------- */

void ObjSeam_TransformVtx(void *vtxp, void *workp, const void *mtxp, const void *ofsp) {
    V4 *vtx = vtxp, *work = workp;
    const M4 *mtx = mtxp;
    const V4 *ofs = ofsp;
    V4 p = vtx[0], n = vtx[1], uv = vtx[2], k, s0, s1, sh, sh2, d0, d1, q0, q1, pos;
    I4 gs;
    float q, wt;
    int i;

    sCur.r[0] = vtx[0];
    sCur.r[3] = v4(4096.0f, 4096.0f, 0.0f, 0.0f);
    /* work[0..3] = light * mtx[0] rows 0-2, light row 3; work[4..7] the same with mtx[1] */
    xload((const M4 *)&work[8]);
    for (i = 0; i < 3; i++) {
        work[i] = xf(mtx[0].r[i]);
    }
    work[3] = work[11];
    for (i = 0; i < 3; i++) {
        work[4 + i] = xf(mtx[1].r[i]);
    }
    work[7] = work[11];

    /* the shade of the normal by each node, blended by the weight in p.w */
    wt = p.w;
    xload((const M4 *)&work[0]);
    s0 = xf(v4(n.x, n.y, n.z, 1.0f));
    xload((const M4 *)&work[4]);
    s1 = xf(v4(n.x, n.y, n.z, 1.0f));
    sh.w = (s0.w - s1.w) * wt;
    sh.x = s1.x + (s0.x - s1.x) * wt;
    sh.y = s1.y + (s0.y - s1.y) * wt;
    sh.z = s1.z + (s0.z - s1.z) * wt;
    k = work[12];
    sh.x = sh.x * k.w + k.w;
    sh2 = v4(sh.x, 0.0f, 1.0f, s1.w);

    /* the position by each node (from its offset), blended, then the screen matrix */
    d0 = v4(p.x - ofs[0].x, p.y - ofs[0].y, p.z - ofs[0].z, p.w - ofs[0].w);
    d1 = v4(p.x - ofs[1].x, p.y - ofs[1].y, p.z - ofs[1].z, p.w - ofs[1].w);
    xload(&mtx[0]);
    q0 = xf(v4(d0.x, d0.y, d0.z, 1.0f));
    xload(&mtx[1]);
    q1 = xf(v4(d1.x, d1.y, d1.z, 1.0f));
    pos = v4(q1.x + (q0.x - q1.x) * wt, q1.y + (q0.y - q1.y) * wt, q1.z + (q0.z - q1.z) * wt, q1.w + (q0.w - q1.w) * wt);
    xload_own(&sScreen);
    pos = xf(pos);
    q = persp(&pos);
    gs = (I4){ftoi(pos.x * 16.0f), ftoi(pos.y * 16.0f), ftoi(pos.z * 16.0f), ftoi(pos.w * 16.0f)};
    if (!(pos.x > 0.0f && pos.y > 0.0f && pos.w > 1.0f && pos.x < 4096.0f && pos.y < 4096.0f)) {
        gs.w = (int32_t)0xFFFF8000u; /* vmfir.w vf10, vi1 (0x8000) */
    }
    vtx[5] = v4(pmul(uv.x, q), pmul(uv.y, q), pmul(uv.z, q), d1.w);
    vtx[6] = v4(pmul(sh2.x, q), 0.0f, q, q1.w);
    memcpy(&vtx[3], &gs, sizeof(gs));
}

/* Vu0_ResetState (the port's): the registers cleared */
void Vu0_ResetState(void) {
    memset(&sCur, 0, sizeof(sCur));
    memset(&sClip, 0, sizeof(sClip));
    memset(&sScreen, 0, sizeof(sScreen));
    memset(&sView, 0, sizeof(sView));
    memset(sStack, 0, sizeof(sStack));
    sSp = 0;
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x11FA10..0x11FE80: integer vectors
 * ---------------------------------------------------------------------------------------------------------------- */

void IVec4_SetZeroW1(I4 *v) { *v = (I4){0, 0, 0, 1}; }
void IVec4_SetZero(I4 *v) { *v = (I4){0, 0, 0, 0}; }
void IVec4_Set(I4 *v, int32_t x, int32_t y, int32_t z, int32_t w) { *v = (I4){x, y, z, w}; }

void IVec3_Set(I4 *v, int32_t x, int32_t y, int32_t z) {
    v->z = z;
    v->x = x;
    v->y = y;
}

void IVec4_Swap(I4 *a, I4 *b) {
    I4 ta = *a, tb = *b;

    *b = ta;
    *a = tb;
}

/* component by component, each stored before the next is read (as the original) */
void IVec4_Add(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x + (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y + (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z + (uint32_t)b->z);
    o->w = (int32_t)((uint32_t)a->w + (uint32_t)b->w);
}

void IVec3_Add(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x + (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y + (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z + (uint32_t)b->z);
}

void IVec4_Sub(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x - (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y - (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z - (uint32_t)b->z);
    o->w = (int32_t)((uint32_t)a->w - (uint32_t)b->w);
}

void IVec3_Sub(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x - (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y - (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z - (uint32_t)b->z);
}

void IVec4_Mul(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x * (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y * (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z * (uint32_t)b->z);
    o->w = (int32_t)((uint32_t)a->w * (uint32_t)b->w);
}

void IVec3_Mul(I4 *o, I4 *a, I4 *b) {
    o->x = (int32_t)((uint32_t)a->x * (uint32_t)b->x);
    o->y = (int32_t)((uint32_t)a->y * (uint32_t)b->y);
    o->z = (int32_t)((uint32_t)a->z * (uint32_t)b->z);
}

void IVec4_Scale(I4 *o, I4 *a, int32_t s) {
    o->x = (int32_t)((uint32_t)a->x * (uint32_t)s);
    o->y = (int32_t)((uint32_t)a->y * (uint32_t)s);
    o->z = (int32_t)((uint32_t)a->z * (uint32_t)s);
    o->w = (int32_t)((uint32_t)a->w * (uint32_t)s);
}

void IVec3_Scale(I4 *o, I4 *a, int32_t s) {
    o->x = (int32_t)((uint32_t)a->x * (uint32_t)s);
    o->y = (int32_t)((uint32_t)a->y * (uint32_t)s);
    o->z = (int32_t)((uint32_t)a->z * (uint32_t)s);
}

/* MIPS div: truncating; INT_MIN / -1 gives INT_MIN */
static int32_t idiv(int32_t a, int32_t s) { return (a == INT32_MIN && s == -1) ? INT32_MIN : a / s; }

/* s == 0 traps on the console (break 7) before anything is written: out untouched */
void IVec4_Div(I4 *o, I4 *a, int32_t s) {
    if (s == 0) {
        return;
    }
    o->x = idiv(a->x, s);
    o->y = idiv(a->y, s);
    o->z = idiv(a->z, s);
    o->w = idiv(a->w, s);
}

void IVec3_Div(I4 *o, I4 *a, int32_t s) {
    if (s == 0) {
        return;
    }
    o->x = idiv(a->x, s);
    o->y = idiv(a->y, s);
    o->z = idiv(a->z, s);
}

void IVec4_Copy(I4 *o, I4 *in) { *o = *in; }

void IVec3_Copy(I4 *o, I4 *in) {
    o->x = in->x;
    o->y = in->y;
    o->z = in->z;
}

void IVec4_ToFloat12(V4 *o, I4 *in) {
    I4 i = *in;

    *o = v4((float)i.x * (1.0f / 4096.0f), (float)i.y * (1.0f / 4096.0f), (float)i.z * (1.0f / 4096.0f), (float)i.w * (1.0f / 4096.0f));
}

void IVec4_ToFloat4(V4 *o, I4 *in) {
    I4 i = *in;

    *o = v4((float)i.x * (1.0f / 16.0f), (float)i.y * (1.0f / 16.0f), (float)i.z * (1.0f / 16.0f), (float)i.w * (1.0f / 16.0f));
}

void IVec4_ToFloat(V4 *o, I4 *in) {
    I4 i = *in;

    *o = v4((float)i.x, (float)i.y, (float)i.z, (float)i.w);
}

static int32_t iclamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (hi < v ? hi : v); }

void IVec4_Clamp(I4 *o, I4 *in, int32_t lo, int32_t hi) {
    o->x = iclamp(in->x, lo, hi);
    o->y = iclamp(in->y, lo, hi);
    o->z = iclamp(in->z, lo, hi);
    o->w = iclamp(in->w, lo, hi);
}

/* x, y, z clamped; out.w = in.w */
void IVec3_Clamp(I4 *o, I4 *in, int32_t lo, int32_t hi) {
    o->x = iclamp(in->x, lo, hi);
    o->y = iclamp(in->y, lo, hi);
    o->z = iclamp(in->z, lo, hi);
    o->w = in->w;
}

void IVec_Stub(void) {}

/* 0x11FE80..: x, y in 12.4 fixed point, w an integer: 0 < x / 16 < 4096, 0 < y / 16 < 4096, w > 0 (z not tested) */
static int gs_in(const I4 *p) { return p->x > 0 && p->y > 0 && p->w > 0 && p->x < 65536 && p->y < 65536; }

int32_t IVec4_InGsRange(I4 *p) { return gs_in(p); }
int32_t IVec4_InGsRange3(I4 *a, I4 *b, I4 *c) { return gs_in(a) && gs_in(b) && gs_in(c); }

/* ORIGINAL BUG: the fourth register is loaded from c again; d is never read */
int32_t IVec4_InGsRange4(I4 *a, I4 *b, I4 *c, I4 *d) {
    (void)d;
    return gs_in(a) && gs_in(b) && gs_in(c);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x11FFE8..0x120A80: sine / cosine, matrices in memory
 * ---------------------------------------------------------------------------------------------------------------- */

/* The original leaves its sine and cosine in vf12 for the hand-written routine that called it; every such routine
   is here and takes them from sc(). */
void Vu0_SinCos(float angle) { (void)angle; }

/* vf1..3 = the unit axes: literals here */
void Vu0_InitAxisRegs(void) {}

void Mtx_StoreIdentity(M4 *m) { *m = kIdentity; }

void Mtx_StoreIdentityRot(M4 *m) {
    m->r[0] = kIdentity.r[0];
    m->r[1] = kIdentity.r[1];
    m->r[2] = kIdentity.r[2];
}

void Mtx_ClearTrans(M4 *m) { m->r[3] = kIdentity.r[3]; }
void Mtx_GetTrans(V4 *v, M4 *m) { *v = m->r[3]; }
void Mtx_SetTrans(M4 *m, V4 *v) { m->r[3] = *v; }

void Vec3_DirToEuler(V4 *out, const V4 *dir);

/* dir = row 2 + (0, 0, 0, 1) (the matrix's rotation applied to (0, 0, 1, 1) with no translation); its pitch and yaw */
void Mtx_AxisZToEuler(V4 *angles, M4 *m) {
    V4 dir = m->r[2];

    dir.w += 1.0f;
    Vec3_DirToEuler(angles, &dir);
}

/* dst rows 0-2 = src rows 0-2 (stored first); dst row 3 = (src row 3 xyz + v xyz, src row 3 w) */
void Mtx_Translate(M4 *dst, M4 *src, V4 *v) {
    V4 t;

    dst->r[0] = src->r[0];
    dst->r[1] = src->r[1];
    dst->r[2] = src->r[2];
    t = src->r[3];
    dst->r[3] = v4(t.x + v->x, t.y + v->y, t.z + v->z, t.w);
}

/* dst = src with row 3 = src * v (v.w used) */
void Mtx_TranslateLocal(M4 *dst, M4 *src, V4 *v) {
    M4 s = *src;

    xload(&s);
    s.r[3] = xf(*v);
    *dst = s;
}

void Mtx_Mul(M4 *dst, M4 *a, M4 *b) { mtx_mul(dst, a, b); }
void Mtx_Copy(M4 *dst, M4 *src) { *dst = *src; }

void Mtx_Transpose(M4 *dst, M4 *src) {
    M4 s = *src, t;
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            (&t.r[i].x)[j] = (&s.r[j].x)[i];
        }
    }
    *dst = t;
}

/* inverse of a rotation + translation: rows 0-2 = the transposed 3x3 (w = 0); row 3 = (-(R^T t), t.w) */
void Mtx_InverseRT(M4 *dst, M4 *src) {
    M4 s = *src;
    V4 t = s.r[3];

    dst->r[0] = v4(s.r[0].x, s.r[1].x, s.r[2].x, 0.0f);
    dst->r[1] = v4(s.r[0].y, s.r[1].y, s.r[2].y, 0.0f);
    dst->r[2] = v4(s.r[0].z, s.r[1].z, s.r[2].z, 0.0f);
    dst->r[3] = v4(0.0f - dot3(s.r[0], t), 0.0f - dot3(s.r[1], t), 0.0f - dot3(s.r[2], t), t.w);
}

/* each row r of src rotated: Z: x' = x c - y s, y' = x s + y c; X: y, z; Y: z' = z c - x s, x' = z s + x c */
void Mtx_RotateZ(M4 *dst, M4 *src, float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(dst, src, 0, 1, a.cos, a.sin);
}

void Mtx_RotateX(M4 *dst, M4 *src, float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(dst, src, 1, 2, a.cos, a.sin);
}

void Mtx_RotateY(M4 *dst, M4 *src, float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(dst, src, 2, 0, a.cos, a.sin);
}

/* Z by angles.z (read before dst is written), then X, then Y */
void Mtx_RotateZXY(M4 *dst, M4 *src, V4 *angles) {
    Mtx_RotateZ(dst, src, angles->z);
    Mtx_RotateX(dst, dst, angles->x);
    Mtx_RotateY(dst, dst, angles->y);
}

void Mtx_RotateXYZ(M4 *dst, M4 *src, V4 *angles) {
    Mtx_RotateX(dst, src, angles->x);
    Mtx_RotateY(dst, dst, angles->y);
    Mtx_RotateZ(dst, dst, angles->z);
}

/* only the diagonal is multiplied (the original's behaviour) */
void Mtx_ScaleDiag(M4 *dst, M4 *src, V4 *v) {
    M4 s = *src;
    V4 k = *v;

    s.r[0].x *= k.x;
    s.r[1].y *= k.y;
    s.r[2].z *= k.z;
    *dst = s;
}

void Mtx_ScaleDiagUniform(M4 *dst, M4 *src, float k) {
    M4 s = *src;

    s.r[0].x *= k;
    s.r[1].y *= k;
    s.r[2].z *= k;
    *dst = s;
}

/* ORIGINAL BUG: the translation copies src's rows over the rotation (harmless only when dst == src) */
void Mtx_RotateZXYTranslate(M4 *dst, M4 *src, V4 *angles, V4 *pos) {
    Mtx_RotateZXY(dst, src, angles);
    Mtx_Translate(dst, src, pos);
}

/* sceVu0CameraMatrix */
void Mtx_MakeCamera(M4 *m, V4 *pos, V4 *zdir, V4 *ydir) {
    M4 m0 = kIdentity;

    normalize3(&m0.r[0], cross3(*ydir, *zdir));
    normalize3(&m0.r[2], *zdir);
    m0.r[1] = cross3(m0.r[2], m0.r[0]);
    Mtx_Translate(&m0, &m0, pos);
    Mtx_InverseRT(m, &m0);
}

/* sceVu0LightColorMatrix */
void Mtx_SetRows(M4 *m, V4 *r0, V4 *r1, V4 *r2, V4 *r3) {
    m->r[0] = *r0;
    m->r[1] = *r1;
    m->r[2] = *r2;
    m->r[3] = *r3;
}

/* sceVu0NormalLightMatrix: rows normalize(-l_i) (w = 0), row 3 = (0, 0, 0, 1), transposed */
void Mtx_MakeNormalLight(M4 *m, V4 *l0, V4 *l1, V4 *l2) {
    M4 t;

    normalize3(&t.r[0], v4(-l0->x, -l0->y, -l0->z, -l0->w));
    normalize3(&t.r[1], v4(-l1->x, -l1->y, -l1->z, -l1->w));
    normalize3(&t.r[2], v4(-l2->x, -l2->y, -l2->z, -l2->w));
    t.r[3] = kIdentity.r[3];
    Mtx_Transpose(m, &t);
}

/* sceVu0ViewScreenMatrix */
void Mtx_MakeViewScreen(M4 *m, float scrz, float ax, float ay, float cx, float cy, float zmin, float zmax, float nearz, float farz) {
    float cz = div_ps2(-zmax * nearz + zmin * farz, farz - nearz);
    float az = div_ps2((farz * nearz) * (zmax - zmin), farz - nearz);
    M4 mp = kIdentity, mt = kIdentity;

    mp.r[0].x = scrz;
    mp.r[1].y = scrz;
    mp.r[2].z = 0.0f;
    mp.r[2].w = 1.0f;
    mp.r[3].z = 1.0f;
    mp.r[3].w = 0.0f;
    mt.r[0].x = ax;
    mt.r[1].y = ay;
    mt.r[2].z = az;
    mt.r[3] = v4(cx, cy, cz, 1.0f);
    mtx_mul(m, &mt, &mp);
}

/* sceVu0DropShadowMatrix (src/port/vu0_a.c has the matrices); lp read completely before m is written */
void Mtx_MakeDropShadow(M4 *m, V4 *lp, float a, float b, float c, int32_t mode) {
    float x = lp->x, y = lp->y, z = lp->z;

    if (mode != 0) { /* point light */
        float d = 1.0f - (a * x + b * y + c * z);

        m->r[0] = v4(a * x + d, a * y, a * z, a);
        m->r[1] = v4(b * x, b * y + d, b * z, b);
        m->r[2] = v4(c * x, c * y, c * z + d, c);
        m->r[3] = v4(-x, -y, -z, d - 1.0f);
    } else { /* parallel light */
        float n = a * x + b * y + c * z;
        float nr = div_ps2(-1.0f, n);

        m->r[0] = v4(nr * (a * x - n), nr * (a * y), nr * (a * z), 0.0f);
        m->r[1] = v4(nr * (b * x), nr * (b * y - n), nr * (b * z), 0.0f);
        m->r[2] = v4(nr * (c * x), nr * (c * y), nr * (c * z - n), 0.0f);
        m->r[3] = v4(nr * -x, nr * -y, nr * -z, nr * -n);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x120A80..0x121008: the current matrix and its stack
 * ---------------------------------------------------------------------------------------------------------------- */

void Vu0Cur_Init(void) {
    sCur = kIdentity;
    sSp = 0;
}

void Vu0Cur_LoadIdentity(void) { sCur = kIdentity; }

/* VU0 memory[vi15++] = rows 0..3; no overflow check, the address wraps after 64 matrices */
void Vu0Cur_Push(void) {
    int i;

    for (i = 0; i < 4; i++) {
        sStack[(uint16_t)sSp++ & 0xFF] = sCur.r[i];
    }
}

void Vu0Cur_Pop(void) {
    int i;

    for (i = 3; i >= 0; i--) {
        sCur.r[i] = sStack[(uint16_t)--sSp & 0xFF];
    }
}

void Vu0Cur_PopN(int32_t n) {
    int32_t i;

    for (i = 0; i < n; i++) {
        Vu0Cur_Pop();
    }
}

void Vu0Cur_ResetStack(void) { sSp = 0; }
int32_t Vu0Cur_GetStackDepth(void) { return sSp / 4; }
int32_t Vu0Cur_IsStackUsed(void) { return Vu0Cur_GetStackDepth() != 0; }

void Vu0Cur_LoadIdentityRot(void) {
    sCur.r[0] = kIdentity.r[0];
    sCur.r[1] = kIdentity.r[1];
    sCur.r[2] = kIdentity.r[2];
}

void Vu0Cur_ClearTrans(void) { sCur.r[3] = kIdentity.r[3]; }
void Vu0Cur_LoadMtx(M4 *m) { sCur = *m; }
void Vu0Cur_StoreMtx(M4 *m) { *m = sCur; }
void Vu0Cur_GetTrans(V4 *v) { *v = sCur.r[3]; }

/* ORIGINAL BUG: row 3 is not cleared, so the translation is added to the axis */
void Vu0Cur_AxisZToEuler(V4 *angles) {
    V4 a = sCur.r[2], t = sCur.r[3];
    V4 dir = v4(a.x + t.x, a.y + t.y, a.z + t.z, a.w + t.w);

    Vec3_DirToEuler(angles, &dir);
}

void Vu0Cur_SetTrans(V4 *v) { sCur.r[3] = *v; }

/* row 3 xyz += v xyz */
void Vu0Cur_Translate(V4 *v) {
    sCur.r[3].x += v->x;
    sCur.r[3].y += v->y;
    sCur.r[3].z += v->z;
}

/* row 3 = current * v */
void Vu0Cur_TranslateLocal(V4 *v) {
    xload_own(&sCur);
    sCur.r[3] = xf(*v);
}

/* current = current * m */
void Vu0Cur_MulMtx(M4 *m) {
    M4 b = *m;

    xload_own(&sCur);
    sCur.r[0] = xf(b.r[0]);
    sCur.r[1] = xf(b.r[1]);
    sCur.r[2] = xf(b.r[2]);
    sCur.r[3] = xf(b.r[3]);
}

/* current = m * current */
void Vu0Cur_MulMtxRev(M4 *m) {
    xload(m);
    sCur.r[0] = xf(sCur.r[0]);
    sCur.r[1] = xf(sCur.r[1]);
    sCur.r[2] = xf(sCur.r[2]);
    sCur.r[3] = xf(sCur.r[3]);
}

void Vu0Cur_Transpose(void) { Mtx_Transpose(&sCur, &sCur); }
void Vu0Cur_InverseRT(void) { Mtx_InverseRT(&sCur, &sCur); }

void Vu0Cur_RotateZ(float angle) { Mtx_RotateZ(&sCur, &sCur, angle); }
void Vu0Cur_RotateX(float angle) { Mtx_RotateX(&sCur, &sCur, angle); }
void Vu0Cur_RotateY(float angle) { Mtx_RotateY(&sCur, &sCur, angle); }

void Vu0Cur_RotateZXY(V4 *angles) {
    Vu0Cur_RotateZ(angles->z);
    Vu0Cur_RotateX(angles->x);
    Vu0Cur_RotateY(angles->y);
}

void Vu0Cur_RotateXYZ(V4 *angles) {
    Vu0Cur_RotateX(angles->x);
    Vu0Cur_RotateY(angles->y);
    Vu0Cur_RotateZ(angles->z);
}

void Vu0Cur_ScaleDiag(V4 *v) { Mtx_ScaleDiag(&sCur, &sCur, v); }
void Vu0Cur_ScaleDiagUniform(float k) { Mtx_ScaleDiagUniform(&sCur, &sCur, k); }

void Vu0Cur_RotateZXYTranslate(V4 *angles, V4 *pos) {
    Vu0Cur_RotateZXY(angles);
    Vu0Cur_Translate(pos);
}

void Vu0Cur_MulVec4(V4 *out, V4 *v) {
    xload_own(&sCur);
    *out = xf(*v);
}

/* out.w = v.w */
void Vu0Cur_MulVec3(V4 *out, V4 *v) {
    V4 in = *v, p;

    xload_own(&sCur);
    p = xf(in);
    p.w = in.w;
    *out = p;
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x240C68..0x240DB8: the stage module's rotations of the current matrix (the opposite sign of Vu0Cur_RotateZ / X)
 * ---------------------------------------------------------------------------------------------------------------- */

void StgVu_RotateZ(float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(&sCur, &sCur, 0, 1, a.cos, -a.sin);
}

void StgVu_RotateX(float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(&sCur, &sCur, 1, 2, a.cos, -a.sin);
}

void StgVu_RotateY(float angle) {
    shz_sincos_t a = sc(angle);
    rot_in_rows(&sCur, &sCur, 2, 0, a.cos, a.sin);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x121008..0x121240: projections by the current matrix
 * ---------------------------------------------------------------------------------------------------------------- */

void Vu0Cur_RotEulerMulVec4(V4 *out, const V4 *angles, const V4 *v) {
    sCur = kIdentity;
    Vu0Cur_RotateZXY((V4 *)angles);
    Vu0Cur_MulVec4(out, (V4 *)v);
}

void Vu0Cur_RotEulerMulVec3(V4 *out, const V4 *angles, const V4 *v) {
    sCur = kIdentity;
    Vu0Cur_RotateZXY((V4 *)angles);
    Vu0Cur_MulVec3(out, (V4 *)v);
}

/* trunc(x / w, y / w, z / w, w) */
void Vu0Cur_ProjectInt(I4 *out, const V4 *v) {
    V4 p;

    xload_own(&sCur);
    p = xf(*v);
    persp(&p);
    *out = (I4){ftoi(p.x), ftoi(p.y), ftoi(p.z), ftoi(p.w)};
}

int Vu0Cur_ProjectPoint(I4 *out, const V4 *v) {
    V4 p;

    xload_own(&sCur);
    p = xf(*v);
    persp(&p);
    *out = to_gs(p);
    return on_screen(p);
}

/* stops after storing the first point off screen (the rest not written); n <= 0 is not handled by the original */
static int project_points(I4 *out, const V4 *v, int n) {
    V4 p;

    for (;;) {
        p = xf(*v++);
        persp(&p);
        *out = to_gs(p);
        if (!on_screen(p)) {
            return 0;
        }
        if (--n == 0) {
            return 1;
        }
        out++;
    }
}

int Vu0Cur_ProjectPoints(I4 *out, const V4 *v, int n) {
    xload_own(&sCur);
    return project_points(out, v, n);
}

/* stq = (uv.x Q, uv.y Q, Q, uv.w), stored before out */
int Vu0Cur_ProjectPointStq(I4 *out, V4 *stqo, const V4 *v, const V4 *uv) {
    V4 t = *uv, p;
    float q;

    xload_own(&sCur);
    p = xf(*v);
    q = persp(&p);
    *stqo = stq(t, q, t.w);
    *out = to_gs(p);
    return on_screen(p);
}

/* ORIGINAL QUIRK: stq.w is whatever a scratch register held (render data): 1 here */
static int project_points_stq(I4 *out, V4 *stqo, const V4 *v, const V4 *uv, int n) {
    V4 p;
    float q;

    for (;;) {
        p = xf(*v++);
        q = persp(&p);
        *stqo = stq(*uv++, q, 1.0f);
        *out = to_gs(p);
        if (!on_screen(p)) {
            return 0;
        }
        if (--n == 0) {
            return 1;
        }
        out++;
        stqo++;
    }
}

int Vu0Cur_ProjectPointsStq(I4 *out, V4 *stqo, const V4 *v, const V4 *uv, int n) {
    xload_own(&sCur);
    return project_points_stq(out, stqo, v, uv, n);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x1212D8..0x121948: the clip, screen and view matrices
 * ---------------------------------------------------------------------------------------------------------------- */

void Vu0Clip_LoadMtx(const M4 *m) { sClip = *m; }
void Vu0Clip_StoreMtx(M4 *m) { *m = sClip; }
void Vu0Clip_SetMulMtx(const M4 *a, const M4 *b) { mtx_mul(&sClip, a, b); }
void Vu0Screen_LoadMtx(const M4 *m) { sScreen = *m; }
void Vu0Screen_StoreMtx(M4 *m) { *m = sScreen; }
void Vu0Screen_SetMulMtx(const M4 *a, const M4 *b) { mtx_mul(&sScreen, a, b); }
void Vu0View_LoadMtx(const M4 *m) { sView = *m; }
void Vu0View_StoreMtx(M4 *m) { *m = sView; }
void Vu0View_SetMulMtx(const M4 *a, const M4 *b) { mtx_mul(&sView, a, b); }

/* view = identity rotated X, Z, Y (rows combined: Y the opposite sign of Vu0Cur_RotateYXZ's), row 3 = (R t, t.w) */
void Vu0View_SetRotTrans(const V4 *rot, const V4 *trans) {
    M4 m = kIdentity;
    V4 t = *trans;
    shz_sincos_t a;

    a = sc(rot->x);
    comb_rows(&m, 1, 2, a.cos, a.sin);
    a = sc(rot->z);
    comb_rows(&m, 0, 1, a.cos, a.sin);
    a = sc(rot->y);
    comb_rows(&m, 0, 2, a.cos, -a.sin);
    xload(&m);
    m.r[3] = xf(t);
    m.r[3].w = t.w;
    sView = m;
}

/* view row 3 = (view * t, t.w), then rotated Y, Z, X */
void Vu0View_ApplyTransRot(const V4 *rot, const V4 *trans) {
    M4 m = sView;
    V4 t = *trans;
    shz_sincos_t a;

    xload_own(&sView);
    m.r[3] = xf(t);
    m.r[3].w = t.w;
    a = sc(rot->y);
    comb_rows(&m, 0, 2, a.cos, -a.sin);
    a = sc(rot->z);
    comb_rows(&m, 0, 1, a.cos, a.sin);
    a = sc(rot->x);
    comb_rows(&m, 1, 2, a.cos, a.sin);
    sView = m;
}

/* the current matrix's rows combined: Y rows 0, 2; X rows 1, 2; Z rows 0, 1 */
void Vu0Cur_RotateYXZ(const V4 *rot) {
    shz_sincos_t a;

    a = sc(rot->y);
    comb_rows(&sCur, 0, 2, a.cos, a.sin);
    a = sc(rot->x);
    comb_rows(&sCur, 1, 2, a.cos, a.sin);
    a = sc(rot->z);
    comb_rows(&sCur, 0, 1, a.cos, a.sin);
}

void Vu0_Stub0(void) {}
void Vu0_Stub1(void) {}
void Vu0_Stub2(void) {}
void Vu0_Stub3(void) {}
void Vu0_Stub4(void) {}
void Vu0_Stub5(void) {}
void Vu0_Stub6(void) {}
void Vu0_Stub7(void) {}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x121950..0x121DA8: the polygon clipper
 * ---------------------------------------------------------------------------------------------------------------- */

void ClipVtx_Set(ClipVtx *vtx, const V4 *pos, const V4 *uv, const V4 *col) {
    V4 p = *pos, t = *uv, c = *col;

    vtx->pos = p;
    vtx->uv = t;
    vtx->col = c;
}

void ClipVtx_Copy(ClipVtx *dst, const ClipVtx *src) { *dst = *src; }

void ClipVtx_SetArray(ClipVtx *vtx, const V4 *pos, const V4 *uv, const V4 *col, int n) {
    if (n == 0) {
        return;
    }
    do {
        ClipVtx_Set(vtx++, pos++, uv++, col++);
    } while (--n != 0);
}

void ClipVtx_CopyArray(ClipVtx *dst, const ClipVtx *src, int n) {
    if (n == 0) {
        return;
    }
    do { /* (as floats: a struct copy is a memcpy call on the SH4) */
        const float *s = &src->pos.x;
        float *d = &dst->pos.x;
        int j;

        for (j = 0; j < 12; j++) {
            d[j] = s[j];
            __asm__("" ::: "memory"); /* (not a memcpy call again: gcc makes such a loop one) */
        }
        dst++;
        src++;
    } while (--n != 0);
}

static inline V4 lerp4(V4 a, V4 b, float t, float u) {
    return v4(a.x * t + b.x * u, a.y * t + b.y * u, a.z * t + b.z * u, a.w * t + b.w * u);
}

/* out = a * t + b * (1 - t) */
void ClipVtx_Lerp(ClipVtx *out, const ClipVtx *a, const ClipVtx *b, float t) {
    float u = 1.0f - t;
    ClipVtx va = *a, vb = *b;

    out->pos = lerp4(va.pos, vb.pos, t, u);
    out->uv = lerp4(va.uv, vb.uv, t, u);
    out->col = lerp4(va.col, vb.col, t, u);
}

/* dist / dot(plane.xyz, a - b) */
float ClipPlane_EdgeParam(const V4 *plane, const ClipVtx *a, const ClipVtx *b, float dist) {
    V4 d = v4(a->pos.x - b->pos.x, a->pos.y - b->pos.y, a->pos.z - b->pos.z, 0.0f);

    return div_ps2(dist, dot3(*plane, d));
}

/* dist[i].x = dot(plane.xyz, pos.xyz) + plane.w; y, z, w are what a scratch register held (only x is read): 0 */
void ClipPlane_DistArray(V4 *dist, const ClipVtx *poly, const V4 *plane, int n) {
    V4 pl;

    if (n == 0) {
        return;
    }
    pl = *plane;
    do {
        const V4 *p = &poly->pos;

        *dist++ = v4(dot4(pl, v4(p->x, p->y, p->z, 1.0f)), 0.0f, 0.0f, 0.0f);
        poly++;
    } while (--n != 0);
}

/* Sutherland-Hodgman against one plane, in place; "outside" is dist < 0. The original's buffers hold 9 vertices. */
static __attribute__((noinline)) int clip_plane_cut(ClipVtx *poly, const V4 *plane, int n) {
    ClipVtx buf[9];
    V4 dist[9];
    int out[12];
    ClipVtx *dst;
    int i, j, nOut;
    float t;

    if (n == 0) {
        return 0;
    }
    ClipPlane_DistArray(dist, poly, plane, n);
    nOut = 0;
    for (i = 0; i < n; i++) {
        out[i] = dist[i].x < 0.0f;
        nOut += out[i];
    }
    if (nOut == 0) {
        return n;
    }
    if (n == nOut) {
        return 0;
    }
    dst = buf;
    j = 1;
    for (i = 0; i < n; i++, j++) {
        ClipVtx *cur = &poly[i], *next;

        if (j >= n) {
            j = 0;
        }
        next = &poly[j];
        if (out[i] == 0) {
            *dst++ = *cur;
            if (out[j] != 0) {
                t = ClipPlane_EdgeParam(plane, cur, next, dist[i].x);
                ClipVtx_Lerp(dst++, next, cur, t);
            }
        } else if (out[j] == 0) {
            t = ClipPlane_EdgeParam(plane, cur, next, dist[i].x);
            ClipVtx_Lerp(dst++, next, cur, t);
        }
    }
    nOut = (int)(dst - buf);
    ClipVtx_CopyArray(poly, buf, nOut);
    return nOut;
}

/* (most polygons are wholly inside or wholly outside: told by their distances alone, as clip_plane_cut tells them,
   without its frame and buffers) */
int ClipPoly_ClipPlane(ClipVtx *poly, const V4 *plane, int n) {
    V4 pl;
    int i, nOut = 0;

    if (n == 0) {
        return 0;
    }
    pl = *plane;
    for (i = 0; i < n; i++) {
        const V4 *p = &poly[i].pos;
        nOut += dot4(pl, v4(p->x, p->y, p->z, 1.0f)) < 0.0f;
    }
    if (nOut == 0) {
        return n;
    }
    if (nOut == n) {
        return 0;
    }
    return clip_plane_cut(poly, plane, n);
}

/* XMTRX loaded: no on-screen test, no return value; stq.w is a scratch register's (render data): 1 here */
static void clip_project(I4 *scr, V4 *stqo, const ClipVtx *poly, int n) {
    V4 p;
    float q;

    do {
        p = xf(poly->pos);
        q = persp(&p);
        *stqo++ = stq(poly->uv, q, 1.0f);
        *scr++ = to_gs(p);
        poly++;
    } while (--n != 0);
}

void ClipPoly_ProjectMtx(I4 *scr, V4 *stqo, const M4 *m, const ClipVtx *poly, int n) {
    xload(m);
    clip_project(scr, stqo, poly, n);
}

void ClipPoly_ProjectCur(I4 *scr, V4 *stqo, const ClipVtx *poly, int n) {
    xload_own(&sCur);
    clip_project(scr, stqo, poly, n);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x121DA8..0x122030
 * ---------------------------------------------------------------------------------------------------------------- */

void Vu0_Init(void) {
    Rand_SeedFloat(0.1234140992f);
    sCur = kIdentity;
    sSp = 0;
    Rand_Init();
}

void Vu0_CheckState(void) {}

void Vec4_SetZeroW1(V4 *v) { *v = v4(0.0f, 0.0f, 0.0f, 1.0f); }
void Vec4_SetZero(V4 *v) { *v = v4(0.0f, 0.0f, 0.0f, 0.0f); }
void Vec4_Set(V4 *v, float x, float y, float z, float w) { *v = v4(x, y, z, w); }

/* w untouched */
void Vec3_Set(V4 *v, float x, float y, float z) {
    v->x = x;
    v->y = y;
    v->z = z;
}

/* out = v / |v| in x y z, w = 0; a zero vector gives (0, 0, 0, 0) */
void Vec3_Normalize(V4 *out, const V4 *v) { normalize3(out, *v); }

void Vec4_Swap(V4 *a, V4 *b) {
    V4 ta = *a, tb = *b;

    *b = ta;
    *a = tb;
}

/* the Vec3 forms keep the first vector's w */
void Vec4_Add(V4 *o, const V4 *a, const V4 *b) { *o = from_shz(shz_vec4_add(to_shz(*a), to_shz(*b))); }

void Vec3_Add(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;

    *o = v4(va.x + vb.x, va.y + vb.y, va.z + vb.z, va.w);
}

void Vec4_Sub(V4 *o, const V4 *a, const V4 *b) { *o = from_shz(shz_vec4_sub(to_shz(*a), to_shz(*b))); }

void Vec3_Sub(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;

    *o = v4(va.x - vb.x, va.y - vb.y, va.z - vb.z, va.w);
}

void Vec4_Mul(V4 *o, const V4 *a, const V4 *b) { *o = from_shz(shz_vec4_mul(to_shz(*a), to_shz(*b))); }

void Vec3_Mul(V4 *o, const V4 *a, const V4 *b) {
    V4 va = *a, vb = *b;

    *o = v4(va.x * vb.x, va.y * vb.y, va.z * vb.z, va.w);
}

void Vec4_Scale(V4 *o, const V4 *v, float s) { *o = from_shz(shz_vec4_scale(to_shz(*v), s)); }

void Vec3_Scale(V4 *o, const V4 *v, float s) {
    V4 va = *v;

    *o = v4(va.x * s, va.y * s, va.z * s, va.w);
}

/* v * (1 / d); d == 0: +/-FLT_MAX */
void Vec4_Div(V4 *o, const V4 *v, float d) {
    V4 va = *v;
    float q = recip(d);

    *o = v4(pmul(va.x, q), pmul(va.y, q), pmul(va.z, q), pmul(va.w, q));
}

void Vec3_Div(V4 *o, const V4 *v, float d) {
    V4 va = *v;
    float q = recip(d);

    *o = v4(pmul(va.x, q), pmul(va.y, q), pmul(va.z, q), va.w);
}

void Vec4_Copy(V4 *dst, const V4 *src) { *dst = *src; }

/* dst.w unchanged */
void Vec3_Copy(V4 *dst, const V4 *src) {
    V4 s = *src;

    dst->x = s.x;
    dst->y = s.y;
    dst->z = s.z;
}

void Mtx_MulVec4(V4 *out, const M4 *m, const V4 *v) {
    V4 in = *v;

    xload(m);
    *out = xf(in);
}

/* the full four-term product, out.w = v.w */
void Mtx_MulVec3(V4 *out, const M4 *m, const V4 *v) {
    V4 in = *v, p;

    xload(m);
    p = xf(in);
    p.w = in.w;
    *out = p;
}

void Vec4_RotateEuler(V4 *out, const V4 *angles, const V4 *v) {
    M4 m;

    Mtx_RotateZXY(&m, (M4 *)&kIdentity, (V4 *)angles);
    Mtx_MulVec4(out, &m, v);
}

/* ------------------------------------------------------------------------------------------------------------------
 * 0x122088..0x122590
 * ---------------------------------------------------------------------------------------------------------------- */

float Vec3_Dot(const V4 *a, const V4 *b) { return dot3(*a, *b); }

/* w = 0 */
void Vec3_Cross(V4 *out, const V4 *a, const V4 *b) { *out = cross3(*a, *b); }

void Vec4_ToFixed12(I4 *out, const V4 *v) {
    V4 in = *v;

    *out = (I4){ftoi(in.x * 4096.0f), ftoi(in.y * 4096.0f), ftoi(in.z * 4096.0f), ftoi(in.w * 4096.0f)};
}

void Vec4_ToFixed4(I4 *out, const V4 *v) {
    V4 in = *v;

    *out = (I4){ftoi(in.x * 16.0f), ftoi(in.y * 16.0f), ftoi(in.z * 16.0f), ftoi(in.w * 16.0f)};
}

void Vec4_ToInt(I4 *out, const V4 *v) {
    V4 in = *v;

    *out = (I4){ftoi(in.x), ftoi(in.y), ftoi(in.z), ftoi(in.w)};
}

void Vec4_ToFixed4XY(I4 *out, const V4 *v) { *out = to_gs(*v); }

/* min(max(c, lo), hi): with lo > hi the result is hi */
static inline float clampf(float c, float lo, float hi) {
    c = c < lo ? lo : c;
    return c > hi ? hi : c;
}

void Vec4_Clamp(V4 *o, const V4 *v, float lo, float hi) {
    V4 a = *v;

    *o = v4(clampf(a.x, lo, hi), clampf(a.y, lo, hi), clampf(a.z, lo, hi), clampf(a.w, lo, hi));
}

void Vec3_Clamp(V4 *o, const V4 *v, float lo, float hi) {
    V4 a = *v;

    *o = v4(clampf(a.x, lo, hi), clampf(a.y, lo, hi), clampf(a.z, lo, hi), a.w);
}

/* a * t + b * (1 - t): t = 1 gives a */
void Vec4_Lerp(V4 *o, const V4 *a, const V4 *b, float t) { *o = lerp4(*a, *b, t, 1.0f - t); }

void Vec3_Lerp(V4 *o, const V4 *a, const V4 *b, float t) {
    V4 va = *a, r = lerp4(va, *b, t, 1.0f - t);

    r.w = va.w;
    *o = r;
}

float Vec3_Length(const V4 *v) { return nat_sqrtf(dot3(*v, *v)); }
float Vec3_LengthSq(const V4 *v) { return dot3(*v, *v); }

float Vec3_Dist(const V4 *a, const V4 *b) {
    V4 d = v4(a->x - b->x, a->y - b->y, a->z - b->z, 0.0f);

    return nat_sqrtf(dot3(d, d));
}

float Vec3_DistSq(const V4 *a, const V4 *b) {
    V4 d = v4(a->x - b->x, a->y - b->y, a->z - b->z, 0.0f);

    return dot3(d, d);
}

/* out.y first, then dir read again (with out == dir the pitch uses the new y); out.w not written */
void Vec3_DirToEuler(V4 *out, const V4 *dir) {
    float len;

    out->y = nat_atan2f(dir->x, dir->z);
    len = nat_sqrtf(dir->x * dir->x + dir->z * dir->z);
    out->x = -nat_atan2f(dir->y, len);
    out->z = 0.0f;
}

void Vec3_DiffToEuler(V4 *out, const V4 *a, const V4 *b) {
    V4 d;

    Vec3_Sub(&d, a, b);
    Vec3_DirToEuler(out, &d);
}

void Mtx_ProjectInt(I4 *out, const M4 *m, const V4 *v) {
    V4 p;

    xload(m);
    p = xf(*v);
    persp(&p);
    *out = (I4){ftoi(p.x), ftoi(p.y), ftoi(p.z), ftoi(p.w)};
}

int Mtx_ProjectPoint(I4 *out, const M4 *m, const V4 *v) {
    V4 p;

    xload(m);
    p = xf(*v);
    persp(&p);
    *out = to_gs(p);
    return on_screen(p);
}

int Mtx_ProjectPoints(I4 *out, const M4 *m, const V4 *v, int n) {
    xload(m);
    return project_points(out, v, n);
}

int Mtx_ProjectPointStq(I4 *out, V4 *stqo, const M4 *m, const V4 *v, const V4 *uv) {
    V4 t = *uv, p;
    float q;

    xload(m);
    p = xf(*v);
    q = persp(&p);
    *stqo = stq(t, q, t.w);
    *out = to_gs(p);
    return on_screen(p);
}

int Mtx_ProjectPointsStq(I4 *out, V4 *stqo, const M4 *m, const V4 *v, const V4 *uv, int n) {
    xload(m);
    return project_points_stq(out, stqo, v, uv, n);
}

void Vu0_Stub8(void) {}

/* out.w = a.w unless said */
void Vec3_AddSub(V4 *o, const V4 *a, const V4 *b, const V4 *c) {
    V4 va = *a, vb = *b, vc = *c;

    *o = v4(va.x + vb.x - vc.x, va.y + vb.y - vc.y, va.z + vb.z - vc.z, va.w);
}

void Vec3_SubAdd(V4 *o, const V4 *a, const V4 *b, const V4 *c) {
    V4 va = *a, vb = *b, vc = *c;

    *o = v4(va.x - vb.x + vc.x, va.y - vb.y + vc.y, va.z - vb.z + vc.z, va.w);
}

/* dir * s + base; out.w = base.w */
void Vec3_ScaleAdd(V4 *o, const V4 *dir, const V4 *base, float s) {
    V4 d = *dir, b = *base;

    *o = v4(d.x * s + b.x, d.y * s + b.y, d.z * s + b.z, b.w);
}

void Vec3_ScaleSub(V4 *o, const V4 *dir, const V4 *base, float s) {
    V4 d = *dir, b = *base;

    *o = v4(d.x * s - b.x, d.y * s - b.y, d.z * s - b.z, b.w);
}

void Vec3_Add4(V4 *o, const V4 *a, const V4 *b, const V4 *c, const V4 *d) {
    V4 va = *a, vb = *b, vc = *c, vd = *d;

    *o = v4(va.x + vb.x + vc.x + vd.x, va.y + vb.y + vc.y + vd.y, va.z + vb.z + vc.z + vd.z, va.w);
}

void Vec4_AddClamp(V4 *o, const V4 *a, const V4 *b, float lo, float hi) {
    V4 va = *a, vb = *b;

    *o = v4(clampf(va.x + vb.x, lo, hi), clampf(va.y + vb.y, lo, hi), clampf(va.z + vb.z, lo, hi), clampf(va.w + vb.w, lo, hi));
}

void Vec3_AddClamp(V4 *o, const V4 *a, const V4 *b, float lo, float hi) {
    V4 va = *a, vb = *b;

    *o = v4(clampf(va.x + vb.x, lo, hi), clampf(va.y + vb.y, lo, hi), clampf(va.z + vb.z, lo, hi), va.w);
}

/*
 * v cos + (axis x v) sin + axis (axis . v)(1 - cos), four components, then out.w = v.w. ORIGINAL BUG (aliasing):
 * v.w is read after out is written, so with out == v the w kept is the sum's own. The axis is not normalised.
 */
void Vec3_RotateAxis(V4 *out, const V4 *v, const V4 *axis, float angle) {
    shz_sincos_t a = sc(angle);
    V4 vv = *v, ax = *axis, b = cross3(ax, vv);
    float k = dot3(ax, vv) * (1.0f - a.cos);

    *out = v4(vv.x * a.cos + b.x * a.sin + ax.x * k, vv.y * a.cos + b.y * a.sin + ax.y * k, vv.z * a.cos + b.z * a.sin + ax.z * k,
              vv.w * a.cos + ax.w * k);
    out->w = v->w;
}

/* ORIGINAL BUG (aliasing): with out == v, w is multiplied by cos(angle) on every call */
void Vec3_RotateX(V4 *out, const V4 *v, float angle) {
    shz_sincos_t a = sc(angle);
    V4 vv = *v;

    *out = v4(vv.x * a.cos + vv.x * (1.0f - a.cos), vv.y * a.cos + -vv.z * a.sin, vv.z * a.cos + vv.y * a.sin, vv.w * a.cos);
    out->w = v->w;
}

void Vec3_RotateY(V4 *out, const V4 *v, float angle) {
    shz_sincos_t a = sc(angle);
    V4 vv = *v;

    *out = v4(vv.x * a.cos + vv.z * a.sin, vv.y * a.cos + vv.y * (1.0f - a.cos), vv.z * a.cos + -vv.x * a.sin, vv.w * a.cos);
    out->w = v->w;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The VU0 R register (the update rule emulators implement; src/port/vu0_b.c)
 * ---------------------------------------------------------------------------------------------------------------- */

static uint32_t bits_of(float f) {
    uint32_t u;

    memcpy(&u, &f, 4);
    return u;
}

void Vu0R_Init(uint32_t *r, float seed) { *r = 0x3F800000u | (bits_of(seed) & 0x007FFFFFu); }

float Vu0R_Next(uint32_t *r) {
    uint32_t b = ((*r >> 4) ^ (*r >> 22)) & 1u;
    float f;

    *r = 0x3F800000u | (((*r << 1) | b) & 0x007FFFFFu);
    memcpy(&f, r, 4);
    return f;
}

float Rand_Float01_Model(uint32_t *r) {
    float f = 0.0f;
    int i;

    for (i = 0; i < 7; i++) {
        f = Vu0R_Next(r);
    }
    Vu0R_Init(r, f);
    for (i = 0; i < 7; i++) {
        f = Vu0R_Next(r);
    }
    return f - 1.0f;
}

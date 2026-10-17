/* The Dreamcast's game code (Makefile.dc's GAME_CFLAGS, -include): vu0_native.c's small vector / matrix routines as
   inline copies, so a call in the game's code is the SH4's instructions in place, as the PS2's VU0 macros were, not
   a call into another file (no LTO). cc_ee.py --dc-inline renames each call of one in a game file to its copy here
   (DC_INLINE: the names); a declaration, a definition or an address taken is left as it is (the out-of-line one).
   Each is vu0_native.c's routine of the same name, as it computes it on the Dreamcast (not make dcsim's): change
   them together. */
#ifndef DC_VEC_INLINE_H
#define DC_VEC_INLINE_H
#include <float.h>
#include <stdint.h>
#include <sh4zam/shz_sh4zam.h>

typedef struct DcvV4 {
    float x, y, z, w;
} DcvV4;

typedef struct DcvM4 {
    DcvV4 r[4];
} DcvM4;

static inline __attribute__((always_inline)) DcvV4 dcv_v4(float x, float y, float z, float w) {
    DcvV4 v = {x, y, z, w};
    return v;
}
static inline __attribute__((always_inline)) shz_vec4_t dcv_to(DcvV4 v) { return shz_vec4_init(v.x, v.y, v.z, v.w); }
static inline __attribute__((always_inline)) DcvV4 dcv_from(shz_vec4_t v) { return dcv_v4(v.x, v.y, v.z, v.w); }

/* (vu0_native.c ps2f, pmul, recip) */
static inline __attribute__((always_inline)) float dcv_ps2f(float r) { return __builtin_fabsf(r) > FLT_MAX ? __builtin_copysignf(FLT_MAX, r) : r; }
static inline __attribute__((always_inline)) float dcv_pmul(float a, float b) { return dcv_ps2f(a * b); }
static inline __attribute__((always_inline)) float dcv_recip(float w) {
    uint32_t u, e;

    __builtin_memcpy(&u, &w, 4);
    e = (u >> 23) & 0xFF;
    if (e == 0) {
        return (u >> 31) ? -FLT_MAX : FLT_MAX;
    }
    if (e < 127 - 60 || e > 127 + 60) {
        return dcv_ps2f(1.0f / w);
    }
    return shz_invf(w);
}

/* (the PS2's lq / sq: four words, loaded then stored; a struct copy here is a memcpy call at -Os, and sh4zam's
   shz_memcpy4 a call too, its SHZ_INLINE only a hint) */
static inline __attribute__((always_inline)) void dcv_Vec4_Copy(void *dst, const void *src) {
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    uint32_t a = s[0], b = s[1], c = s[2], e = s[3];

    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}
static inline __attribute__((always_inline)) void dcv_Vec3_Copy(void *dst, const void *src) {
    DcvV4 s = *(const DcvV4 *)src;
    DcvV4 *d = (DcvV4 *)dst;

    d->x = s.x;
    d->y = s.y;
    d->z = s.z;
}
static inline __attribute__((always_inline)) void dcv_Vec4_SetZero(void *v) { *(DcvV4 *)v = dcv_v4(0.0f, 0.0f, 0.0f, 0.0f); }
static inline __attribute__((always_inline)) void dcv_Vec4_Set(void *v, float x, float y, float z, float w) { *(DcvV4 *)v = dcv_v4(x, y, z, w); }
static inline __attribute__((always_inline)) void dcv_Vec3_Set(void *v, float x, float y, float z) {
    DcvV4 *d = (DcvV4 *)v;

    d->x = x;
    d->y = y;
    d->z = z;
}

static inline __attribute__((always_inline)) void dcv_Vec4_Add(void *o, const void *a, const void *b) {
    *(DcvV4 *)o = dcv_from(shz_vec4_add(dcv_to(*(const DcvV4 *)a), dcv_to(*(const DcvV4 *)b)));
}
static inline __attribute__((always_inline)) void dcv_Vec4_Sub(void *o, const void *a, const void *b) {
    *(DcvV4 *)o = dcv_from(shz_vec4_sub(dcv_to(*(const DcvV4 *)a), dcv_to(*(const DcvV4 *)b)));
}
static inline __attribute__((always_inline)) void dcv_Vec4_Mul(void *o, const void *a, const void *b) {
    *(DcvV4 *)o = dcv_from(shz_vec4_mul(dcv_to(*(const DcvV4 *)a), dcv_to(*(const DcvV4 *)b)));
}
static inline __attribute__((always_inline)) void dcv_Vec4_Scale(void *o, const void *v, float s) {
    *(DcvV4 *)o = dcv_from(shz_vec4_scale(dcv_to(*(const DcvV4 *)v), s));
}
static inline __attribute__((always_inline)) void dcv_Vec3_Add(void *o, const void *a, const void *b) {
    DcvV4 va = *(const DcvV4 *)a, vb = *(const DcvV4 *)b;

    *(DcvV4 *)o = dcv_v4(va.x + vb.x, va.y + vb.y, va.z + vb.z, va.w);
}
static inline __attribute__((always_inline)) void dcv_Vec3_Sub(void *o, const void *a, const void *b) {
    DcvV4 va = *(const DcvV4 *)a, vb = *(const DcvV4 *)b;

    *(DcvV4 *)o = dcv_v4(va.x - vb.x, va.y - vb.y, va.z - vb.z, va.w);
}
static inline __attribute__((always_inline)) void dcv_Vec3_Mul(void *o, const void *a, const void *b) {
    DcvV4 va = *(const DcvV4 *)a, vb = *(const DcvV4 *)b;

    *(DcvV4 *)o = dcv_v4(va.x * vb.x, va.y * vb.y, va.z * vb.z, va.w);
}
static inline __attribute__((always_inline)) void dcv_Vec3_Scale(void *o, const void *v, float s) {
    DcvV4 va = *(const DcvV4 *)v;

    *(DcvV4 *)o = dcv_v4(va.x * s, va.y * s, va.z * s, va.w);
}
static inline __attribute__((always_inline)) void dcv_Vec4_Div(void *o, const void *v, float d) {
    DcvV4 va = *(const DcvV4 *)v;
    float q = dcv_recip(d);

    *(DcvV4 *)o = dcv_v4(dcv_pmul(va.x, q), dcv_pmul(va.y, q), dcv_pmul(va.z, q), dcv_pmul(va.w, q));
}
static inline __attribute__((always_inline)) void dcv_Vec3_Div(void *o, const void *v, float d) {
    DcvV4 va = *(const DcvV4 *)v;
    float q = dcv_recip(d);

    *(DcvV4 *)o = dcv_v4(dcv_pmul(va.x, q), dcv_pmul(va.y, q), dcv_pmul(va.z, q), va.w);
}
static inline __attribute__((always_inline)) float dcv_Vec3_Dot(const void *a, const void *b) {
    DcvV4 va = *(const DcvV4 *)a, vb = *(const DcvV4 *)b;

    return shz_vec3_dot(shz_vec3_init(va.x, va.y, va.z), shz_vec3_init(vb.x, vb.y, vb.z));
}

/* XMTRX = m (4-aligned), then XMTRX * v */
static inline __attribute__((always_inline)) void dcv_Mtx_MulVec4(void *out, const void *m, const void *v) {
    DcvV4 in = *(const DcvV4 *)v;

    shz_xmtrx_load_unaligned_4x4((const float *)m);
    *(DcvV4 *)out = dcv_from(shz_xmtrx_transform_vec4(dcv_to(in)));
}
static inline __attribute__((always_inline)) void dcv_Mtx_MulVec3(void *out, const void *m, const void *v) {
    DcvV4 in = *(const DcvV4 *)v, p;

    shz_xmtrx_load_unaligned_4x4((const float *)m);
    p = dcv_from(shz_xmtrx_transform_vec4(dcv_to(in)));
    p.w = in.w;
    *(DcvV4 *)out = p;
}
#endif

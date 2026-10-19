/* The Dreamcast's libm in place of newlib's for what the game calls most (profiled, drawing off: ~2.5% of the
   instructions): sinf, cosf and sincosf on the SH4's FSCA (sh4zam's shz_sincosf: the angle in 1/65536 turns, the
   result to ~2^-21), and the square root acosf / asinf / powf take inside newlib (__ieee754_sqrtf: there a software
   loop) on FSQRT, correctly rounded as before. These definitions are the program's: libm's of the same names are
   not linked. src/sys/math3d.c's slerps do not take their small angles' sines from here (__DREAMCAST__). */
#include <math.h>
#include <sh4zam/shz_sh4zam.h>

__attribute__((used)) float __ieee754_sqrtf(float x) { return __builtin_sqrtf(x); }

__attribute__((used)) float sqrtf(float x) { return __builtin_sqrtf(x); }

__attribute__((used)) float sinf(float x) { return shz_sincosf(x).sin; }

__attribute__((used)) float cosf(float x) { return shz_sincosf(x).cos; }

__attribute__((used)) void sincosf(float x, float *s, float *c) {
    shz_sincos_t r = shz_sincosf(x);
    *s = r.sin;
    *c = r.cos;
}

/*
 * Forced into every game source of the PC build (-include port_compat.h), in front of the game's own headers.
 * The game sources under src/ and include/ are shared with the matching decompilation and are not edited for the
 * port unless there is no other way; differences between the PS2 compiler and a host compiler are bridged here.
 */
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

#define PORT 1

/* The game's types: on the PS2 `long` is 64 bits. Take over include/types.h. */
#define TYPES_H
typedef signed char s8;
typedef unsigned char u8;
typedef short s16;
typedef unsigned short u16;
typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;
typedef float f32;

/* Functions the matching build takes from assembly: the port supplies them elsewhere (see docs/port/). */
#define INCLUDE_ASM_H
#define INCLUDE_ASM(FOLDER, NAME)
#define INCLUDE_RODATA(FOLDER, NAME)
#define RODATA_ALIGN16()
#define LIT4_WORD(NAME, HEX)
#define ASM_STUB_BEGIN() extern int asm_stub_begin_
#define ASM_STUB_END() extern int asm_stub_end_
/* A declaration under another symbol's name (an asm label), as the object file spells it: with the prefix the
   platform puts in front of C names (none on the PC, '_' on the SH-4's ELF). */
#define PORT_STR_(x) #x
#define PORT_STR(x) PORT_STR_(x)
#define ASM_NAME(name) __asm__(PORT_STR(__USER_LABEL_PREFIX__) name)

/* Copying / clearing a 4-aligned struct as words: on the SH-4 a struct copy or memset of more than a few words is a
   call into newlib. (The barrier keeps gcc from making the loop a memcpy / memset call again.) PORT_ZERO(&x) and
   PORT_COPY(d, s, n) (n of *d) check that the size is whole words. */
static inline __attribute__((always_inline)) void Port_Zero32(void *d, unsigned bytes) {
    unsigned *o = (unsigned *)d, j;
    for (j = 0; j < bytes / 4; j++) {
        o[j] = 0;
        __asm__("" ::: "memory");
    }
}
static inline __attribute__((always_inline)) void Port_Copy32(void *d, const void *s, unsigned bytes) {
    unsigned *o = (unsigned *)d, j;
    const unsigned *i = (const unsigned *)s;
    for (j = 0; j < bytes / 4; j++) {
        o[j] = i[j];
        __asm__("" ::: "memory");
    }
}
#define PORT_WORDS_(x) (sizeof(x) + 0 * sizeof(char[sizeof(x) % 4 == 0 ? 1 : -1]))
#define PORT_ZERO(p) Port_Zero32((p), PORT_WORDS_(*(p)))
#define PORT_COPY(d, s, n) Port_Copy32((d), (s), (n) * PORT_WORDS_(*(d)))

/* C library calls of the game that must not go to the host's versions (see port/src/plat_mem.c). */
#define malloc Port_Malloc

#endif

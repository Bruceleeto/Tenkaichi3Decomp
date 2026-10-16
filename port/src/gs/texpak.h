#ifndef TEXPAK_H
#define TEXPAK_H
#include <stdint.h>

/* The Dreamcast's converted textures (tex.pak, written by make -f Makefile.dc dc-assets: port/tools/dc_tex.c), by
   the hash of their upload (Gs_SrcKey). */
int TexPak_Find(uint64_t key, uint32_t *w, uint32_t *h, uint32_t *bits); /* its index; -1 = not in the pack */
uint32_t TexPak_Bytes(int index); /* its data's size: the indices (twiddled; 4 or 8 bits each), texels, or a palette's colours */
/* Its data read a piece at a time through the pack's one buffer, TEXPAK_PIECE bytes (no block its size): put(ctx,
   piece, at, n) for each, `at` where it is in the data, n a multiple of `align` but for the last. 0 = not read
   (the file failed, said if `align` does not fit the buffer). */
#define TEXPAK_PIECE 8192
int TexPak_Stream(int index, uint32_t align, void (*put)(void *ctx, const uint8_t *piece, uint32_t at, uint32_t n), void *ctx);
int TexPak_ReadInto(int index, void *dst, uint32_t bytes); /* all of it into dst (bytes: TexPak_Bytes); 0 = not read */
uint32_t TexPak_Psm(int index); /* a palette's format (bits 32: w colours, as GS memory holds them) */

#endif

#ifndef TEXPAK_H
#define TEXPAK_H
#include <stdint.h>

/* The Dreamcast's converted textures (tex.pak, written by make -f Makefile.dc dc-assets: port/tools/dc_tex.c), by
   the hash of their upload (Gs_SrcKey). */
int TexPak_Find(uint64_t key, uint32_t *w, uint32_t *h, uint32_t *bits); /* its index; -1 = not in the pack */
void *TexPak_Read(int index); /* its indices (twiddled; 4 or 8 bits each), malloc'd; NULL = not read */

#endif

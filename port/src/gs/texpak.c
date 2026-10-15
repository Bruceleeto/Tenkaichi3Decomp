/*
 * tex.pak: the disc's 4-bit and 8-bit textures as the PVR takes them, converted ahead of time (port/tools/dc_tex.c,
 * which describes the file). The index is read once (20 bytes a texture); a texture's data when it is asked for.
 * BT3_TEXPAK=<file> reads another one, BT3_TEXPAK= none.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texpak.h"

typedef struct PakEnt {
    uint64_t key;
    uint32_t offset;
    uint16_t w, h;
    uint8_t bits, psm;
} PakEnt;

static FILE *sFile;
static PakEnt *sEnt;
static uint32_t sCount;
static int sOpened;

static void pak_open(void) {
#ifdef __DREAMCAST__
    const char *path = "/cd/tex.pak";
#else
    const char *path = "port/build/dc/assets/tex.pak";
#endif
    uint8_t head[8], rec[20];
    uint32_t i;

    sOpened = 1;
    if (getenv("BT3_TEXPAK") != NULL) { path = getenv("BT3_TEXPAK"); }
    if (path[0] == '\0' || (sFile = fopen(path, "rb")) == NULL) { return; }
    if (fread(head, 1, 8, sFile) != 8 || memcmp(head, "BT3T", 4) != 0) { goto bad; }
    memcpy(&sCount, head + 4, 4);
    sEnt = malloc(sCount * sizeof(PakEnt));
    if (sEnt == NULL) { goto bad; }
    for (i = 0; i < sCount; i++) {
        if (fread(rec, 1, 20, sFile) != 20) { goto bad; }
        memcpy(&sEnt[i].key, rec, 8);
        memcpy(&sEnt[i].offset, rec + 8, 4);
        memcpy(&sEnt[i].w, rec + 12, 2);
        memcpy(&sEnt[i].h, rec + 14, 2);
        sEnt[i].bits = rec[16];
        sEnt[i].psm = rec[17];
    }
    fprintf(stderr, "bt3: %u converted textures (%s)\n", (unsigned)sCount, path);
    return;
bad:
    fprintf(stderr, "bt3: %s is not a texture pack\n", path);
    free(sEnt);
    sEnt = NULL;
    sCount = 0;
    fclose(sFile);
    sFile = NULL;
}

int TexPak_Find(uint64_t key, uint32_t *w, uint32_t *h, uint32_t *bits) {
    uint32_t lo = 0, hi;

    if (!sOpened) { pak_open(); }
    hi = sCount;
    while (lo < hi) { /* (sorted by key) */
        uint32_t m = (lo + hi) / 2;
        if (sEnt[m].key < key) { lo = m + 1; }
        else { hi = m; }
    }
    if (lo == sCount || sEnt[lo].key != key) { return -1; }
    *w = sEnt[lo].w;
    *h = sEnt[lo].h;
    *bits = sEnt[lo].bits;
    return (int)lo;
}

uint32_t TexPak_Psm(int index) { return sEnt[index].psm; }

void *TexPak_Read(int index) {
    const PakEnt *e = &sEnt[index];
    uint32_t bytes = (uint32_t)e->w * e->h * e->bits / 8;
    void *buf = malloc(bytes);

    if (buf == NULL || fseek(sFile, (long)e->offset, SEEK_SET) != 0 || fread(buf, 1, bytes, sFile) != bytes) {
        free(buf);
        return NULL;
    }
    return buf;
}

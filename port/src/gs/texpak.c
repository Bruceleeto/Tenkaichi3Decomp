/*
 * tex.pak: the disc's 4-bit and 8-bit textures as the PVR takes them, converted ahead of time (port/tools/dc_tex.c,
 * which describes the file). The index is read once (20 bytes a texture on disc, 10 kept); a texture's data
 * when it is asked for, a piece at a time through one buffer (TexPak_Stream): no block the texture's size.
 * BT3_TEXPAK=<file> reads another one, BT3_TEXPAK= none.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texpak.h"

/* The index, 10 bytes a texture (it was 24 in memory; 25,758 of them): the keys, sorted, and beside each its size
   and format in 16 bits (log2 w - 4: 3 bits, log2 h: 4, bits 4 / 8 / 16 / 32: 2, psm 0 / 0x13 / 0x14 / 0x80: 2).
   The data is in the file in the index's order, one texture after another (dc_tex.c), so a texture's offset is
   the one of every 32nd (sMark) plus the sizes of those between. A pack of another shape is said not to be one. */
static const uint8_t kPsm[4] = {0x00, 0x13, 0x14, 0x80};

static FILE *sFile;
static uint64_t *sKey;
static uint16_t *sInfo;
static uint32_t *sMark;
static uint32_t sCount;
static int sOpened;

static uint32_t ent_w(uint32_t i) { return 16u << (sInfo[i] & 7); }
static uint32_t ent_h(uint32_t i) { return 1u << ((sInfo[i] >> 3) & 15); }
static uint32_t ent_bits(uint32_t i) { return 4u << ((sInfo[i] >> 7) & 3); }
static uint32_t ent_bytes(uint32_t i) { return ent_w(i) * ent_h(i) * ent_bits(i) / 8; }

/* the code of n in a table of powers of two from lo (n itself one of them, below lo << count), or -1 */
static int log_code(uint32_t n, uint32_t lo, int count) {
    int k;
    for (k = 0; k < count; k++) {
        if (n == lo << k) { return k; }
    }
    return -1;
}

static void pak_open(void) {
#ifdef __DREAMCAST__
    extern const char *Port_FileRoot(void); /* plat_file.c: /cd, else /pc */
    char root[80];
    const char *path = root;
    snprintf(root, sizeof(root), "%s/tex.pak", Port_FileRoot());
#else
    const char *path = "port/build/dc/assets/tex.pak";
#endif
    uint8_t head[8], rec[20];
    uint32_t i, next;

    sOpened = 1;
    if (getenv("BT3_TEXPAK") != NULL) { path = getenv("BT3_TEXPAK"); }
    if (path[0] == '\0' || (sFile = fopen(path, "rb")) == NULL) { return; }
    if (fread(head, 1, 8, sFile) != 8 || memcmp(head, "BT3T", 4) != 0) { goto bad; }
    memcpy(&sCount, head + 4, 4);
    sKey = malloc(sCount * sizeof(*sKey));
    sInfo = malloc(sCount * sizeof(*sInfo));
    sMark = malloc((sCount / 32 + 1) * sizeof(*sMark));
    if (sKey == NULL || sInfo == NULL || sMark == NULL) { goto bad; }
    next = 8 + 20 * sCount;
    for (i = 0; i < sCount; i++) {
        uint32_t offset;
        uint16_t w, h;
        int cw, ch, cb, cp;

        if (fread(rec, 1, 20, sFile) != 20) { goto bad; }
        memcpy(&sKey[i], rec, 8);
        memcpy(&offset, rec + 8, 4);
        memcpy(&w, rec + 12, 2);
        memcpy(&h, rec + 14, 2);
        cw = log_code(w, 16, 7);
        ch = log_code(h, 1, 16);
        cb = log_code(rec[16], 4, 4);
        for (cp = 0; cp < 4 && kPsm[cp] != rec[17]; cp++) {
        }
        if (cw < 0 || ch < 0 || cb < 0 || cp == 4 || offset != next || (i != 0 && sKey[i] <= sKey[i - 1])) {
            fprintf(stderr, "bt3: %s: texture %u is not as the index keeps it (size, format, place or order)\n", path, (unsigned)i);
            goto bad;
        }
        sInfo[i] = (uint16_t)(cw | ch << 3 | cb << 7 | cp << 9);
        if ((i & 31) == 0) { sMark[i / 32] = offset; }
        next = offset + ent_bytes(i);
    }
    fprintf(stderr, "bt3: %u converted textures (%s)\n", (unsigned)sCount, path);
    return;
bad:
    fprintf(stderr, "bt3: %s is not a texture pack\n", path);
    free(sKey);
    free(sInfo);
    free(sMark);
    sKey = NULL;
    sInfo = NULL;
    sMark = NULL;
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
        if (sKey[m] < key) { lo = m + 1; }
        else { hi = m; }
    }
    if (lo == sCount || sKey[lo] != key) { return -1; }
    *w = ent_w(lo);
    *h = ent_h(lo);
    *bits = ent_bits(lo);
    return (int)lo;
}

uint32_t TexPak_Psm(int index) { return kPsm[(sInfo[index] >> 9) & 3]; }

uint32_t TexPak_Bytes(int index) { return ent_bytes((uint32_t)index); }

/* where index's data is in the file */
static uint32_t ent_offset(uint32_t i) {
    uint32_t offset = sMark[i / 32], j;
    for (j = i & ~31u; j < i; j++) {
        offset += ent_bytes(j);
    }
    return offset;
}

/* the pack's one buffer (32-aligned, for the store queues pvr_txr_load copies with) */
static uint8_t sPiece[TEXPAK_PIECE] __attribute__((aligned(32)));

int TexPak_Stream(int index, uint32_t align, void (*put)(void *ctx, const uint8_t *piece, uint32_t at, uint32_t n), void *ctx) {
    uint32_t bytes = ent_bytes((uint32_t)index), step, at, n;

    if (align == 0 || align > TEXPAK_PIECE) {
        fprintf(stderr, "bt3: ERROR: tex.pak: texture %d asked for in pieces of %u bytes; the buffer holds %u\n", index, (unsigned)align,
                (unsigned)TEXPAK_PIECE);
        return 0;
    }
    step = TEXPAK_PIECE - TEXPAK_PIECE % align;
    if (fseek(sFile, (long)ent_offset((uint32_t)index), SEEK_SET) != 0) {
        return 0;
    }
    for (at = 0; at < bytes; at += n) {
        n = bytes - at < step ? bytes - at : step;
        if (fread(sPiece, 1, n, sFile) != n) {
            return 0;
        }
        put(ctx, sPiece, at, n);
    }
    return 1;
}

int TexPak_ReadInto(int index, void *dst, uint32_t bytes) {
    if (bytes != ent_bytes((uint32_t)index) || fseek(sFile, (long)ent_offset((uint32_t)index), SEEK_SET) != 0) {
        return 0;
    }
    return fread(dst, 1, bytes, sFile) == bytes;
}

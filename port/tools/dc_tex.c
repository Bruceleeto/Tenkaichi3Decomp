/*
 * The Dreamcast's textures, converted from the disc's files ahead of time (make dc-assets): every texture file the
 * game uploads (TexFile, include/sys/gfxm_b_b.h), wherever it is (alone, inside a resource file's offset table,
 * BPE-packed), its 4-bit and 8-bit pixels as the PVR takes them: indices in twiddled order (PAL4: two to a byte,
 * the first in the low nibble; PAL8 or a VQ index map: one to a byte). The game uploads most of them as 32-bit
 * pixels, so each upload packet goes through GS memory as the GS would store it and is read back in the texture's
 * own format. Palettes too (every entry's, and the palettes kept as entries of their own): their colours as GS
 * memory holds them, in the order the indices count. The ones the game changes while it runs, it still sends.
 *
 * One pack, keyed by a hash of the upload's image data (the first width x height pixels of it, as the GS got them:
 * what port/src/gs/gs_core.c keeps of every 32-bit upload, Gs_SrcKey), so a texture is found however the game
 * loaded its file:
 *     "BT3T" u32 count, then count x {u64 key; u32 offset; u16 w, h; u8 bits, psm, 0, 0} sorted by key, then data
 * (a palette: bits 32, w its colours (16 or 256), h 1, psm its format; one u32 a colour)
 * The 32-, 24- and 16-bit textures too, as 16-bit PVR texels in twiddled order (bits 16, psm the entry's | the PVR
 * format << 6: 0 ARGB1555, 1 RGB565, 2 ARGB4444), keyed by the upload's hash ^ KEY_DIRECT: the 24- and 16-bit ones
 * opaque (the game's TEXA, gfxm_e.c: 0x80 both), a 32-bit one 1555 when its alpha is only none or all. One that
 * fits in a GS page also as 8-bit indices read from where it lies (key ^ KEY_T8; bits 8, w x h the page's blocks it
 * covers, in rows): the game draws its light (btl_obj.c, block 0x3D40) as an 8-bit texture.
 * Run it again whenever this changes; nothing else depends on its output.
 *
 *     dc_tex OUT.pak DIR...
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- GS memory: where a pixel lives (the hardware's tables, as port/src/gs/gs_core.c) ---- */
static uint32_t sVram[1 << 20];
static const uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t kBlock16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t kCol32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}, {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
                                     {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47}, {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t kCol16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27}, {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59}, {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91}, {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123}, {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
static const uint8_t kCol8First[4][16] = {{0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54}, {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
                                          {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23}, {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31}};
static const uint8_t kCol4First[4][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63}};
static uint8_t sCol8[16][16];
static uint16_t sCol4[16][32];

static void tables_init(void) {
    int y, x;
    for (y = 0; y < 16; y++) {
        int col = y >> 2;
        for (x = 0; x < 16; x++) { sCol8[y][x] = (uint8_t)(kCol8First[y & 3][(col & 1) ? (x ^ 4) : x] + col * 64); }
        for (x = 0; x < 32; x++) { sCol4[y][x] = (uint16_t)(kCol4First[y & 3][(col & 1) ? (x ^ 4) : x] + col * 128); }
    }
}

/* The pixel (x, y) of a buffer at block bp, width bw (in 64s), format psm: read (write = 0) or written. */
static uint32_t vram_rw(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, int write, uint32_t v) {
    uint32_t a, *w;
    if (bw == 0) { bw = 1; }
    switch (psm) {
    case 0x02: case 0x0A:
        a = ((bp + ((y >> 1) & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + kBlock16[(y >> 3) & 7][(x >> 4) & 3]) << 7) + kCol16[y & 7][x & 15];
        w = &sVram[(a >> 1) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFFFu << ((a & 1) * 16))) | (v & 0xFFFF) << ((a & 1) * 16); return 0; }
        return (*w >> ((a & 1) * 16)) & 0xFFFF;
    case 0x13:
        a = ((bp + ((y >> 1) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + sCol8[y & 15][x & 15];
        w = &sVram[(a >> 2) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFFu << ((a & 3) * 8))) | (v & 0xFF) << ((a & 3) * 8); return 0; }
        return (*w >> ((a & 3) * 8)) & 0xFF;
    case 0x14:
        a = ((bp + ((y >> 2) & ~0x1Fu) * (bw >> 1) + ((x >> 2) & ~0x1Fu) + kBlock16[(y >> 4) & 7][(x >> 5) & 3]) << 9) + sCol4[y & 15][x & 31];
        w = &sVram[(a >> 3) & 0xFFFFF];
        if (write) { *w = (*w & ~(0xFu << ((a & 7) * 4))) | (v & 0xF) << ((a & 7) * 4); return 0; }
        return (*w >> ((a & 7) * 4)) & 0xF;
    default:
        a = ((bp + (y & ~0x1Fu) * bw + ((x >> 1) & ~0x1Fu) + kBlock32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + kCol32[y & 7][x & 7];
        w = &sVram[a & 0xFFFFF];
        switch (psm) {
        case 0x01: if (write) { *w = (*w & 0xFF000000u) | (v & 0xFFFFFF); return 0; } return *w & 0xFFFFFF;
        case 0x1B: if (write) { *w = (*w & 0x00FFFFFFu) | v << 24; return 0; } return *w >> 24;
        case 0x24: if (write) { *w = (*w & 0xF0FFFFFFu) | (v & 15) << 24; return 0; } return (*w >> 24) & 15;
        case 0x2C: if (write) { *w = (*w & 0x0FFFFFFFu) | (v & 15) << 28; return 0; } return *w >> 28;
        default: if (write) { *w = v; return 0; } return *w;
        }
    }
}

static int psm_bits(uint32_t psm) {
    switch (psm) {
    case 0x00: return 32;
    case 0x01: return 24;
    case 0x02: case 0x0A: return 16;
    case 0x13: case 0x1B: return 8;
    case 0x14: case 0x24: case 0x2C: return 4;
    default: return 0;
    }
}

/* An upload packet (a VIF DIRECT, then GIF tags: TRXPOS / TRXREG by A+D, then IMAGE) into GS memory at block dbp,
   with BITBLTBUF's width and format from the entry (`blt`, BITBLTBUF bits 48..63); write = 0: only read through.
   0 = not such a packet. */
static uint64_t sKey;              /* upload(): the hash of the image data */
static uint32_t sSx, sSy, sRw, sRh; /* and its rectangle */
static int upload(const uint8_t *p, uint32_t size, uint32_t blt, uint32_t dbp, int write) {
    uint32_t dbw = blt & 63, dpsm = (blt >> 8) & 63, bits = (uint32_t)psm_bits(dpsm);
    uint32_t sx = 0, sy = 0, rw = 0, rh = 0, x = 0, y = 0, i, n, at = 0, images = 0;
    uint32_t want = 0;
    if (bits == 0) { return 0; }
    sKey = 0xCBF29CE484222325ull;
    if (size >= 16 && (p[15] & 0x7F) == 0x50) { at = 16; } /* VIF DIRECT */
    while (at + 16 <= size) {
        uint64_t tag, regs;
        uint32_t nloop, flg, nreg;
        memcpy(&tag, p + at, 8);
        memcpy(&regs, p + at + 8, 8);
        nloop = tag & 0x7FFF;
        flg = (tag >> 58) & 3;
        nreg = (tag >> 60) & 15;
        at += 16;
        if (flg == 0 && nreg == 1 && (regs & 15) == 0xE) { /* A+D */
            for (i = 0; i < nloop && at + 16 <= size; i++, at += 16) {
                uint64_t d;
                memcpy(&d, p + at, 8);
                if (p[at + 8] == 0x51) { sx = (d >> 32) & 0x7FF; sy = (d >> 48) & 0x7FF; }
                if (p[at + 8] == 0x52) { rw = d & 0xFFF; rh = (d >> 32) & 0xFFF; }
            }
        } else if (flg == 2) { /* IMAGE */
            if (rw == 0 || rh == 0) { return 0; }
            sSx = sx; sSy = sy; sRw = rw; sRh = rh;
            n = nloop * 16;
            if (at + n > size) { n = size - at; }
            if (images == 0) { want = rw * rh * bits / 8; }
            for (i = 0; i < n && want != 0; i++, want--) { sKey = (sKey ^ p[at + i]) * 0x100000001B3ull; }
            for (i = 0; i < n * 8 / bits && y < rh; i++) {
                uint32_t v;
                switch (bits) {
                case 32: memcpy(&v, p + at + i * 4, 4); break;
                case 24: v = p[at + i * 3] | p[at + i * 3 + 1] << 8 | p[at + i * 3 + 2] << 16; break;
                case 16: v = p[at + i * 2] | p[at + i * 2 + 1] << 8; break;
                case 8: v = p[at + i]; break;
                default: v = (p[at + i / 2] >> ((i & 1) * 4)) & 15; break;
                }
                if (write) { vram_rw(dbp, dbw, dpsm, sx + x, sy + y, 1, v); }
                if (++x == rw) { x = 0; y++; }
            }
            at += nloop * 16;
            images++;
        } else {
            return images != 0;
        }
    }
    return images != 0;
}

/* ---- the PVR's twiddled order (as port/src/gs/gs_pvr.c) ---- */
static uint32_t spread_bits(uint32_t x) {
    x &= 0x3FF;
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}
static uint32_t twiddle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, bits = 0, s, idx;
    for (s = m; s > 1; s >>= 1) { bits++; }
    idx = spread_bits(v & (m - 1)) | (spread_bits(u & (m - 1)) << 1);
    if (w != h) { idx |= ((w > h ? u : v) >> bits) << (2 * bits); }
    return idx;
}

/* ---- the pack ---- */
typedef struct Out {
    uint64_t key;
    uint32_t offset;
    uint16_t w, h;
    uint8_t bits, psm; /* a palette: bits 32, w its colours, h 1, psm its format (CT32 / CT16) */
} Out;
static Out *sOut;
static uint32_t sOutN, sOutCap;
static FILE *sData;
static uint32_t sDataBytes;
static unsigned sStat[9]; /* texture entries, 4-bit, 8-bit, other formats, bad packets, duplicates, files with textures, BPE parts,
                             palettes */

#define KEY_DIRECT 0x9E3779B97F4A7C15ull /* (as port/src/gs/gs_pvr.c) */
#define KEY_T8     0xC2B2AE3D27D4EB4Full

static void out_add(uint64_t key, uint32_t w, uint32_t h, uint32_t bits, uint32_t psm, const void *data, uint32_t bytes);

/* A 32-, 24- or 16-bit texture uploaded at block 0, as the PVR takes it; and, if it fits in a page, its 8-bit
   view. */
static void emit_direct(uint64_t key, uint32_t psm, uint32_t tbw, uint32_t tw, uint32_t th) {
    uint32_t w = tw < 8 ? 8 : tw, h = th < 8 ? 8 : th, x, y, fmt = psm == 0x00 ? 0 : 1, bytes;
    uint16_t *buf = calloc(w * h, 2);
    if (psm == 0x00) { /* 1555 unless some alpha is neither none nor all (0x80 and over) */
        for (y = 0; y < th && fmt == 0; y++) {
            for (x = 0; x < tw; x++) {
                uint32_t a = vram_rw(0, tbw, psm, x, y, 0, 0) >> 24;
                if (a != 0 && a < 0x80) { fmt = 2; break; }
            }
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t v = vram_rw(0, tbw, psm, x % tw, y % th, 0, 0), r, g, b, a;
            if (psm == 0x02 || psm == 0x0A) { r = (v & 31) << 3; g = (v >> 5 & 31) << 3; b = (v >> 10 & 31) << 3; a = 0x80; }
            else { r = v & 255; g = v >> 8 & 255; b = v >> 16 & 255; a = psm == 0x00 ? v >> 24 : 0x80; }
            buf[twiddle_index(x, y, w, h)] = (uint16_t)(fmt == 0 ? (a >= 0x80) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3
                                                      : fmt == 1 ? (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
                                                      : (a >= 0x80 ? 15 : a >> 3) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
        }
    }
    out_add(key ^ KEY_DIRECT, w, h, 16, psm | fmt << 6, buf, w * h * 2);
    free(buf);
    bytes = tw * th * (uint32_t)psm_bits(psm) / 8;
    if (bytes <= 8192) { /* the 8-bit view: the blocks it covers (256 bytes each), placed as an 8-bit page places them */
        uint32_t n = (bytes + 255) / 256, b, vw = 16, vh = 16;
        uint8_t *v8;
        for (b = 0; b < 32; b++) { /* (kBlock32 is also the 8-bit page's: 8 x 4 blocks of 16 x 16) */
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    if (kBlock32[y][x] == b && b < n) {
                        if ((x + 1) * 16 > vw) { vw = (x + 1) * 16; }
                        if ((y + 1) * 16 > vh) { vh = (y + 1) * 16; }
                    }
                }
            }
        }
        while (vw & (vw - 1)) { vw += vw & -vw; }
        while (vh & (vh - 1)) { vh += vh & -vh; }
        v8 = malloc(vw * vh);
        for (y = 0; y < vh; y++) {
            for (x = 0; x < vw; x++) { v8[y * vw + x] = (uint8_t)vram_rw(0, 2, 0x13, x, y, 0, 0); }
        }
        out_add(key ^ KEY_T8, vw, vh, 8, 0x13, v8, vw * vh);
        free(v8);
    }
}

static void emit(uint64_t key, uint32_t psm, uint32_t tbw, uint32_t tw, uint32_t th) {
    uint32_t w = tw < 8 ? 8 : tw, h = th < 8 ? 8 : th, bits = psm == 0x14 ? 4 : 8, bytes = w * h * bits / 8, x, y;
    uint8_t *buf;
    buf = calloc(1, bytes);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t v = vram_rw(0, tbw, psm, x % tw, y % th, 0, 0), t = twiddle_index(x, y, w, h);
            if (bits == 4) { buf[t >> 1] |= (uint8_t)(v << ((t & 1) * 4)); }
            else { buf[t] = (uint8_t)v; }
        }
    }
    out_add(key, w, h, bits, psm, buf, bytes);
    free(buf);
}

static void out_add(uint64_t key, uint32_t w, uint32_t h, uint32_t bits, uint32_t psm, const void *data, uint32_t bytes) {
    if (sOutN == sOutCap) {
        sOutCap = sOutCap ? sOutCap * 2 : 4096;
        sOut = realloc(sOut, sOutCap * sizeof(Out));
    }
    sOut[sOutN].key = key;
    sOut[sOutN].offset = sDataBytes;
    sOut[sOutN].w = (uint16_t)w;
    sOut[sOutN].h = (uint16_t)h;
    sOut[sOutN].bits = (uint8_t)bits;
    sOut[sOutN].psm = (uint8_t)psm;
    sOutN++;
    fwrite(data, 1, bytes, sData);
    sDataBytes += bytes;
}

/* A palette upload (16x16: 256 colours, 8x2: 16), its colours in the order the indices count (256: bits 3 and 4
   exchanged, CSM1), as GS memory holds them: the game's own expansion (TEXA) stays the renderer's. */
static void pal_emit(const uint8_t *p, uint32_t size, uint32_t blt) {
    uint32_t dpsm = (blt >> 8) & 63, n, i, col[256];
    if ((dpsm != 0x00 && dpsm != 0x02 && dpsm != 0x0A) || !upload(p, size, blt, 0, 1)) { return; }
    n = sRw == 16 && sRh == 16 ? 256 : sRw == 8 && sRh == 2 ? 16 : 0;
    if (n == 0) { return; }
    for (i = 0; i < n; i++) {
        uint32_t j = n == 16 ? i : (i & ~0x18u) | (i & 8) << 1 | (i & 0x10) >> 1;
        col[i] = vram_rw(0, blt & 63, dpsm, sSx + j % sRw, sSy + j / sRw, 0, 0);
    }
    sStat[8]++;
    out_add(sKey, n, 1, 32, dpsm, col, n * 4);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* DC_TEX_DUMP=dir: some of the textures as pictures (PPM), with their palettes, to see that they read right. */
static const char *sDump;
static unsigned sDumped;
static void dump(const uint8_t *e, const uint8_t *b, uint32_t n, uint32_t psm, uint32_t tbw, uint32_t tw, uint32_t th) {
    uint32_t clut = rd32(e + 4) * 4, csize = rd32(e + 12), pal[256], i, x, y;
    char path[4096];
    FILE *f;
    if (clut == 0 || csize > n - clut || !upload(b + clut, csize, rd32(e + 0x1C), 0x3800, 1)) { return; }
    for (i = 0; i < (psm == 0x14 ? 16u : 256u); i++) { /* (8-bit: bits 3 and 4 of the index exchanged, CSM1) */
        uint32_t j = psm == 0x14 ? i : (i & ~0x18u) | (i & 8) << 1 | (i & 0x10) >> 1;
        pal[i] = vram_rw(0x3800, 1, 0, j % (psm == 0x14 ? 8 : 16), j / (psm == 0x14 ? 8 : 16), 0, 0);
    }
    snprintf(path, sizeof(path), "%s/%02u_%ubit_%ux%u.ppm", sDump, sDumped++, psm == 0x14 ? 4 : 8, tw, th);
    f = fopen(path, "wb");
    if (f == NULL) { return; }
    fprintf(f, "P6\n%u %u\n255\n", tw, th);
    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            uint32_t c = pal[vram_rw(0, tbw, psm, x, y, 0, 0)];
            fputc(c & 255, f); fputc(c >> 8 & 255, f); fputc(c >> 16 & 255, f);
        }
    }
    fclose(f);
}

/* DC_TEX_XML=file: the catalogue, every file and what is in it (the parts, as far as they are understood). */
static FILE *sXml;
static int sXd;
static void xml(int open, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void xml(int open, const char *fmt, ...) {
    va_list ap;
    if (sXml == NULL) { return; }
    if (open < 0) { sXd--; }
    fprintf(sXml, "%*s", sXd * 2, "");
    va_start(ap, fmt);
    vfprintf(sXml, fmt, ap);
    va_end(ap);
    fputc('\n', sXml);
    if (open > 0) { sXd++; }
}
/* GS image transfers in a part: an A+D TRXDIR (0x53) with BITBLTBUF (0x50) just before it, as every upload
   packet of the game has. A part none of whose bytes holds one can't send pixels to the GS. */
static uint32_t gs_uploads(const uint8_t *b, uint32_t n) {
    uint32_t i, j, count = 0;
    for (i = 48; i + 16 <= n; i += 16) {
        if (rd32(b + i + 8) != 0x53 || rd32(b + i + 12) != 0) { continue; }
        for (j = 1; j <= 3; j++) {
            if (rd32(b + i - j * 16 + 8) == 0x50 && rd32(b + i - j * 16 + 12) == 0) { count++; break; }
        }
    }
    return count;
}

/* Every 16-byte frame a PS2 SPU ADPCM frame: filter 0..4, shift 0..12, flags 0..7. */
static int spu_adpcm(const uint8_t *b, uint32_t n) {
    uint32_t i;
    if (n < 1024 || n % 16 != 0) { return 0; }
    for (i = 0; i < 16; i++) { if (b[i] != 0) { return 0; } } /* (a sample body starts with a frame of zeros) */
    for (i = 0; i < n; i += 16) {
        if ((b[i] >> 4) > 4 || (b[i] & 15) > 12 || b[i + 1] > 7) { return 0; }
    }
    return 1;
}

static uint32_t sUnknownUploads;
static long sFileId;          /* the game's id of the file being read (File_OpenById), 0 for pzs3us0 */
static const char *sRole;     /* what the code takes the part being read for, NULL = not known by its place */
static char sRoleBuf[160];

/* What the game reads part i (1-based, as BObjFile_GetEntry / BtlStage_GetFileMember + 3 count) of a top-level
   file for, by the file's id (include/battle/battle_work.h). NULL: not a file whose parts are known by place. */
static const char *file_role(long id, uint32_t i) {
    static const struct { uint32_t from, to; const char *what; } chara[] = { /* BtlObj_BindTables (bobj_a.c) */
        { 1, 1, "model textures (BObjMdl.unk38)" },
        { 2, 2, "body volumes (BobjBodyData, BtlObj+0x54)" },
        { 3, 3, "model (pmdl)" },
        { 4, 0xB, "mouth set A (BObjMdl.mouthA)" },
        { 0xC, 0xC, "BObjMdl.unk04" },
        { 0xD, 0xD, "eye textures (BObjMdl.eyes)" },
        { 0xE, 0x11, "mouth set B (BObjMdl.mouthB)" },
        { 0x12, 0x12, "character parameters (BtlParam, BtlObj+0x91C)" },
        { 0x13, 0x13, "attack records (BtlAtkRecord, BtlObj+0x920)" },
        { 0x14, 0x14, "ki blasts (BtlKiBlastData, BtlObj+0x924)" },
        { 0x15, 0x15, "moves (BtlMoveData, BtlObj+0x928)" },
        { 0x16, 0x16, "pre-fight talk table (btl_seq.c talkTbl, BtlObj+0x93C)" },
        { 0x17, 0x17, "hair and cloth chains (BObjChainParam, BtlObj+0x940)" },
        { 0x18, 0x18, "super attacks (BtlSuperData, BtlObj+0x92C)" },
        { 0x19, 0x19, "skills (BtlSkillData, BtlObj+0x930)" },
        { 0x1A, 0x1A, "not read by the game" },
        { 0x1B, 0x1B, "AI parameters (BtlObj+0x934)" },
        { 0x1C, 0x1C, "camera cuts (ChrCamCutDef, BtlObj+0x938)" },
        { 0x1D, 0x24, "motions (BObjMdl.unk720, unk8D0)" },
        { 0x25, 0x28, "camera animations (BObjMdl.unk8F0)" },
        { 0x29, 0x2B, "camera animations (BObjMdl.unk98)" },
        { 0x2C, 0x2C, "not read by the game" },
        { 0x2D, 0x34, "text (BObjMdl.unkA4: entry 0x2D + gProgress->unk00)" },
        { 0x35, 0x98, "lip sync, first voice language (BObjMdl.lip)" },
        { 0x99, 0xFC, "lip sync, second voice language (BObjMdl.lip)" },
    }, stage[] = {
        { 1, 1, "stage (StgModel_InitStage)" },
        { 2, 2, "stage data (StgDataA, BtlStage_BindData)" },
        { 3, 5, "camera animations (BtlStage_GetFileMember 0..2)" },
        { 6, 6, "scrolling sheet textures (EftStageScroll_Load)" },
        { 7, 7, "scrolling sheet: parameters and mesh (EftStageScroll_Load)" },
        { 9, 9, "animated objects (StgAnimTable, StgModel_BindAnims)" },
        { 0x13, 0x13, "stage effect points (MEF0, gBtlStageFxRes)" },
        { 0x18, 0x18, "paths (StgPathTable, BtlStage_BindPaths)" },
    };
    uint32_t k;
    if (id == 2) { /* common data 0 (common.c: gCommonRes->data[0]) */
        return i == 1 ? "light (BtlObjLight, btl_obj.c)"
             : i == 4 ? "AI think data (AiThData, btl_ai_cond.c)"
             : i == 3 || (i >= 5 && i <= 9) ? "roster tables (gBtlChars->tbl, btl_char_mgr.c)" : NULL;
    }
    if (id == 5) { return i == 1 || i == 2 ? "font (Font_SetFont, col_c.c: gCommonRes->boot)" : NULL; }
    if (id >= 0x590 && id < 0xBDA && (id - 0x590) % 10 < 8) {
        for (k = 0; k < sizeof(chara) / sizeof(chara[0]); k++) {
            if (i >= chara[k].from && i <= chara[k].to) { return chara[k].what; }
        }
        return NULL;
    }
    if (id >= 0x590 && id < 0xBDA && (id - 0x590) % 10 == 8) { return "motion (BObjMdl.anims, BtlObj_BindTables)"; }
    if (id >= 0x590 && id < 0xBDA && (id - 0x590) % 10 == 9) {
        return i <= 5 ? "BObjMdl.unk84 (BtlObj_BindTables)"
             : i == 6 ? "character effect pack (BtlScene_GetCharPackEntry)" : NULL;
    }
    if (id >= 0x171 && id < 0x1BF) {
        for (k = 0; k < sizeof(stage) / sizeof(stage[0]); k++) {
            if (i >= stage[k].from && i <= stage[k].to) { return stage[k].what; }
        }
        return "not read by the game (no BtlStage_GetFileMember or header word read of it)";
    }
    return NULL;
}

/* Whether the part at b is an emitter set's header (EftSetHead, battle/eft_h.h): the group table after it adds
   up to its part count. Its resources follow it in the same table (EftEmit_LoadSet): for each group of a kind
   other than 0, nRes texture sets, then one parameter block per part (two, a and b, but for kinds 2 and 14). */
static int eft_set(const uint8_t *b, uint32_t n, uint32_t *after) {
    uint32_t g, nGroups, parts = 0, next = 0;
    if (n < 0x20) { return 0; }
    nGroups = b[4];
    if (nGroups == 0 || nGroups > 16 || 0x20 + nGroups * 0x20 > n) { return 0; }
    for (g = 0; g < nGroups; g++) {
        const uint8_t *d = b + 0x20 + g * 0x20;
        if (d[0] > 0x20) { return 0; }
        parts += d[1];
        if (d[0] != 0) { next += d[2] + d[1] * (d[0] == 2 || d[0] == 0xE ? 1u : 2u); }
    }
    if (parts != b[5] || parts == 0) { return 0; }
    *after = next;
    return 1;
}

static void xml_unknown(const uint8_t *b, uint32_t n) {
    char head[3 * 16 + 1] = "";
    uint32_t i;
    if (sXml == NULL) { return; }
    for (i = 0; i < 16 && i < n; i++) { sprintf(head + i * 3, "%02X%s", b[i], i + 1 < 16 && i + 1 < n ? " " : ""); }
    {   /* what it is, where its first bytes tell (none of these hold textures: the game's are in texture tables) */
        const char *kind = n >= 4 && b[0] == 0x80 && b[1] == 0x00 ? "adx sound"
                         : n >= 4 && memcmp(b, "pmdl", 4) == 0 ? "model (pmdl)"
                         : n >= 4 && memcmp(b, "IECS", 4) == 0 ? "sound bank header"
                         : n >= 2 && b[0] == 0xFF && b[1] == 0xFE ? "text (UTF-16)"
                         : n >= 4 && memcmp(b, "LIPS", 4) == 0 ? "lip sync"
                         : n >= 4 && memcmp(b, "CMAn", 4) == 0 ? "camera animation"
                         : n >= 4 && memcmp(b, "FOD\x11", 4) == 0 ? "menu layout (FOD)"
                         : n >= 4 && memcmp(b, "V000", 4) == 0 ? "sprite animation (V000)"
                         : n >= 8 && memcmp(b, "\0\0\0\x01\xD0\x07\0\0", 8) == 0 ? "item table (GetWin_Init part 10)"
                         : n >= 4 && memcmp(b, "5tnF", 4) == 0 ? "font (Font_SetFont)"
                         : n >= 4 && memcmp(b, "cmdl", 4) == 0 ? "stage change model (cmdl, EftBurst_RelocateModel)"
                         : n >= 4 && memcmp(b, "GSCF", 4) == 0 ? "battle script (GSCF, gsc.c)"
                         : n >= 4 && memcmp(b, "MAPD", 4) == 0 ? "stage data (StgDataA, BtlStage_BindData)"
                         : n >= 4 && memcmp(b, "MEF0", 4) == 0 ? "stage effect points (MEF0)"
                         : n >= 4 && memcmp(b, "EVPp", 4) == 0 ? "paths (StgPathTable)"
                         : n >= 4 && b[3] == 0x60 && b[2] == 0x00 ? "VU1 geometry (DMA chain)"
                         : n >= 0x94 && b[0] == 0x61 ? "motion (bobj_a.h BobjAnim)"
                         : spu_adpcm(b, n) ? "sound samples (SPU ADPCM)"
                         : NULL;
        uint32_t up = gs_uploads(b, n);
        if (sRole != NULL) { kind = sRole; }
        sUnknownUploads += up;
        if (kind != NULL) { xml(0, "<data kind=\"%s\" size=\"%u\" gs_uploads=\"%u\"/>", kind, n, up); }
        else { xml(0, "<unknown size=\"%u\" head=\"%s\" gs_uploads=\"%u\"/>", n, head, up); }
    }
}
static const char *psm_name(uint32_t psm) {
    switch (psm) {
    case 0x00: return "CT32"; case 0x01: return "CT24"; case 0x02: return "CT16"; case 0x0A: return "CT16S";
    case 0x13: return "T8"; case 0x14: return "T4"; case 0x1B: return "T8H"; case 0x24: return "T4HL"; case 0x2C: return "T4HH";
    default: return "?";
    }
}

/* A texture file's header at b + hdr, its offsets counted from b (n bytes; the game's Res_RelocateOffsets takes the
   two apart: a table inside a resource file can count from the whole file): its textures out. 0 = not one. */
static int texfile(const uint8_t *b, uint32_t n, uint32_t hdr) {
    uint32_t count, ent, i, ok = 0;
    if (n < 0x20 || hdr > n - 0x20) { return 0; }
    count = rd32(b + hdr);
    ent = rd32(b + hdr + 4) * 4;
    if (count == 0 || count > 1024 || ent < 0x10 || ent > n || (n - ent) / 0x40 < count) { return 0; }
    for (i = 0; i < count; i++) { /* every entry first, its packet read through, so that a table that only looks like one
                                      is left alone */
        const uint8_t *e = b + ent + i * 0x40;
        uint32_t pix = rd32(e) * 4, size = rd32(e + 8), clut = rd32(e + 4) * 4, csize = rd32(e + 12);
        uint64_t t0;
        memcpy(&t0, e + 0x30, 8);
        if (pix == 0 && clut == 0) { continue; }
        if (pix > n || size > n - pix || clut > n || csize > n - clut || ((t0 >> 26) & 15) > 10 || ((t0 >> 30) & 15) > 10 ||
            psm_bits((t0 >> 20) & 63) == 0 || (pix != 0 && size != 0 && !upload(b + pix, size, rd32(e + 0x18), 0, 0))) {
            return 0;
        }
        ok += pix != 0 && size != 0; /* (a table of nothing but palettes, or of nothing, is not taken for one) */
    }
    if (ok == 0) { return 0; }
    xml(1, "<textures at=\"0x%X\" count=\"%u\">", hdr, count);
    for (i = 0; i < count; i++) {
        const uint8_t *e = b + ent + i * 0x40;
        uint32_t pix = rd32(e) * 4, size = rd32(e + 8), clut = rd32(e + 4) * 4, csize = rd32(e + 12), psm;
        uint64_t t0;
        memcpy(&t0, e + 0x30, 8);
        psm = (t0 >> 20) & 63;
        if (pix != 0 && size != 0) {
            xml(0, "<tex i=\"%u\" psm=\"%s\" w=\"%u\" h=\"%u\" clut=\"%s\" as=\"%s\"/>", i, psm_name(psm), 1u << ((t0 >> 26) & 15),
                1u << ((t0 >> 30) & 15), clut != 0 && csize != 0 ? "yes" : "no",
                psm == 0x13 || psm == 0x14 ? "indexed" : psm <= 0x0A ? "direct" : "NOT CONVERTED");
        } else if (clut != 0 && csize != 0) {
            xml(0, "<tex i=\"%u\" palette-only=\"yes\"/>", i);
        }
        if (clut != 0 && csize != 0) { pal_emit(b + clut, csize, rd32(e + 0x1C)); }
        if (pix == 0 || size == 0) { continue; }
        sStat[0]++;
        if (psm != 0x13 && psm != 0x14) {
            sStat[3]++;
            pal_emit(b + pix, size, rd32(e + 0x18)); /* (it may be a palette kept as an entry of its own) */
            if (psm == 0x00 || psm == 0x01 || psm == 0x02 || psm == 0x0A) {
                memset(sVram, 0, sizeof(sVram));
                if (upload(b + pix, size, rd32(e + 0x18), 0, 1)) {
                    emit_direct(sKey, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
                }
            }
            continue;
        }
        memset(sVram, 0, sizeof(sVram));
        if (!upload(b + pix, size, rd32(e + 0x18), 0, 1)) { sStat[4]++; continue; }
        sStat[psm == 0x14 ? 1 : 2]++;
        if (sDump != NULL && sDumped < 40 && sStat[psm == 0x14 ? 1 : 2] % 997 == 1) {
            dump(e, b, n, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
        }
        emit(sKey, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
    }
    xml(-1, "</textures>");
    return 1;
}

static uint8_t *bpe(const uint8_t *src, uint32_t n, uint32_t *outSize) { /* src/sys/bpe.c, with bounds */
    uint8_t left[256], right[256], stack[256], *buf, *out;
    uint32_t size = rd32(src), pack = rd32(src + 4);
    const uint8_t *in = src + 8, *end = src + 8 + pack;
    if (pack > n - 8 || size > 64u << 20) { return NULL; }
    buf = out = malloc(size + 1);
    while (in < end) {
        int c = 0, count = *in++, i, blockSize;
        for (i = 0; i < 256; i++) { left[i] = (uint8_t)i; }
        for (;;) {
            if (count > 127) { c += count - 127; count = 0; }
            if (c == 256) { break; }
            for (i = 0; i <= count && c < 256; i++, c++) {
                if (in >= end) { goto bad; }
                left[c] = *in++;
                if (c != left[c]) {
                    if (in >= end) { goto bad; }
                    right[c] = *in++;
                }
            }
            if (c == 256) { break; }
            if (in >= end) { goto bad; }
            count = *in++;
        }
        if (in + 2 > end) { goto bad; }
        blockSize = in[0] << 8 | in[1];
        in += 2;
        i = 0;
        for (;;) {
            if (i != 0) { c = stack[--i]; }
            else {
                if (blockSize-- == 0) { break; }
                if (in >= end) { goto bad; }
                c = *in++;
            }
            if (c == left[c]) {
                if ((uint32_t)(out - buf) >= size) { goto bad; }
                *out++ = (uint8_t)c;
            } else if (i < 254) {
                stack[i++] = right[c];
                stack[i++] = left[c];
            } else { goto bad; }
        }
    }
    if ((uint32_t)(out - buf) != size) { goto bad; }
    *outSize = size;
    return buf;
bad:
    free(buf);
    return NULL;
}

/* Whatever b holds: a texture file, a BPE-packed part, or an offset table of parts (count, then the parts' byte
   offsets), each looked through in turn: as whatever it holds itself, or as a texture file counting from b;
   anything else is searched for texture tables. */
/* Something of a format of its own (a stage's or a model's resources): every texture table in it, wherever its
   header is, counting from b (as the game finds them: by an offset in the format's own header). */
/* Texture tables anywhere in b (a format of its own): counting from themselves (as the game's (res, res, res)) or
   from b. */
/* A font (src/battle/col_c.c FontData, "5tnF"): one texture entry at +0x0C, its packets at byte offsets from the font
   (Font_SetFont), 4-bit with four palettes of 16 colours in one 16x16 upload. */
static int font(const uint8_t *b, uint32_t n) {
    const uint8_t *e;
    uint32_t pix, size, clut, csize, psm;
    uint64_t t0;
    if (n < 0x24 || memcmp(b, "5tnF", 4) != 0 || rd32(b + 0xC) > n - 0x40) { return 0; }
    e = b + rd32(b + 0xC);
    pix = rd32(e), size = rd32(e + 8), clut = rd32(e + 4), csize = rd32(e + 12);
    memcpy(&t0, e + 0x30, 8);
    psm = (t0 >> 20) & 63;
    if (pix > n || size > n - pix || clut > n || csize > n - clut || psm != 0x14 || ((t0 >> 26) & 15) > 10 || ((t0 >> 30) & 15) > 10) { return 0; }
    xml(1, "<textures at=\"0x%X\" count=\"1\" format=\"font\">", rd32(b + 0xC));
    xml(0, "<tex i=\"0\" psm=\"%s\" w=\"%u\" h=\"%u\" clut=\"yes\" as=\"indexed\"/>", psm_name(psm), 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
    pal_emit(b + clut, csize, rd32(e + 0x1C));
    memset(sVram, 0, sizeof(sVram));
    if (upload(b + pix, size, rd32(e + 0x18), 0, 1)) {
        sStat[0]++;
        sStat[1]++;
        emit(sKey, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
    } else {
        sStat[4]++;
    }
    xml(-1, "</textures>");
    return 1;
}

static int search(const uint8_t *b, uint32_t n) {
    uint32_t h;
    int found = 0;
    for (h = 0; h + 0x20 <= n; h += 4) {
        uint32_t count = rd32(b + h), ent = rd32(b + h + 4) * 4;
        if (count == 0 || count > 1024 || ent < 0x10) { continue; }
        if (ent <= n - h && (n - h - ent) / 0x40 >= count && texfile(b + h, n - h, 0)) { found = 1; continue; }
        if (ent <= n && (n - ent) / 0x40 >= count) { found |= texfile(b, n, h); }
    }
    return found;
}

/* Whatever b is: a texture file, BPE, a table of parts (u32 count, then count offsets; the game's MPACK_AT takes
   the low two bits off), or a format of its own searched through. Each into the catalogue. */
static int scan(const uint8_t *b, uint32_t n, int depth) {
    uint32_t count, i, found = 0;
    if (n < 16 || depth > 6) { xml_unknown(b, n); return 0; }
    if (texfile(b, n, 0) || font(b, n)) { return 1; }
    if (rd32(b + 4) <= n - 8 && rd32(b + 4) + 8 + 2048 > n && rd32(b) < (64u << 20)) { /* (padded
                                                                      to 0x40 in an archive, to a sector as a file) */
        uint32_t size;
        uint8_t *raw = bpe(b, n, &size);
        if (raw != NULL) {
            sStat[7]++;
            xml(1, "<bpe packed=\"%u\" size=\"%u\">", n, size);
            found = (uint32_t)scan(raw, size, depth + 1);
            xml(-1, "</bpe>");
            free(raw);
            return (int)found;
        }
    }
    count = rd32(b);
    for (i = 0; count != 0 && count <= 0x10000 && 4 + count * 4 <= n && i < count; i++) {
        uint32_t o = rd32(b + 4 + i * 4) & ~3u;
        if (o != 0 && (o < 4 + count * 4 || o > n)) { break; } /* (one at the end: an empty part) */
    }
    if (count == 0 || count > 0x10000 || 4 + count * 4 > n || i < count) {
        found = (uint32_t)search(b, n);
        if (!found) { xml_unknown(b, n); }
        return (int)found;
    }
    xml(1, "<table count=\"%u\">", count);
    {
    const char *parent = sRole;
    uint32_t setLeft = 0;
    for (i = 0; i < count; i++) {
        uint32_t o = rd32(b + 4 + i * 4) & ~3u, end = n, j;
        if (o == 0 || o == n) { continue; }
        if (i + 1 < count && (rd32(b + 8 + i * 4) & ~3u) == o) { continue; } /* empty: its offset is the next one's
                                                                                (BtlStage_GetFileMember) */
        for (j = 0; j < count; j++) { /* the part runs to the next part */
            uint32_t q = rd32(b + 4 + j * 4) & ~3u;
            if (q > o && q < end) { end = q; }
        }
        xml(1, "<part i=\"%u\" at=\"0x%X\" size=\"%u\">", i + 1, o, end - o);
        {
            uint32_t after;
            sRole = depth == 0 ? file_role(sFileId, i + 1) : parent;
            if (eft_set(b + o, end - o, &after)) { sRole = "effect emitter set header (EftSetHead, EftEmit_LoadSet)"; setLeft = after; }
            else if (setLeft != 0) {
                setLeft--;
                snprintf(sRoleBuf, sizeof(sRoleBuf), "effect emitter resource or parameters (EftEmit_LoadSet)");
                sRole = sRoleBuf;
            }
        }
        if (texfile(b, n, o)) { found = 1; } /* (counting from the whole file: Res_RelocateOffsets with another base) */
        else { found |= (uint32_t)scan(b + o, end - o, depth + 1); }
        sRole = parent;
        xml(-1, "</part>");
    }
    }
    xml(-1, "</table>");
    return (int)found;
}

static int key_cmp(const void *a, const void *b) {
    const Out *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key;
}

int main(int argc, char **argv) {
    char tmp[4096];
    FILE *f;
    uint32_t i, kept = 0, base;
    int d;

    if (argc < 3) {
        fprintf(stderr, "usage: dc_tex OUT.pak DIR...\n");
        return 2;
    }
    tables_init();
    sDump = getenv("DC_TEX_DUMP");
    if (getenv("DC_TEX_XML") != NULL && (sXml = fopen(getenv("DC_TEX_XML"), "w")) != NULL) { fprintf(sXml, "<disc>\n"); sXd = 1; }
    snprintf(tmp, sizeof(tmp), "%s.data", argv[1]);
    sData = fopen(tmp, "wb+");
    if (sData == NULL) { perror(tmp); return 1; }
    for (d = 2; d < argc; d++) {
        DIR *dir = opendir(argv[d]);
        struct dirent *de;
        if (dir == NULL) { perror(argv[d]); return 1; }
        while ((de = readdir(dir)) != NULL) {
            char path[4096];
            uint8_t *b;
            long n;
            if (de->d_name[0] == '.') { continue; }
            snprintf(path, sizeof(path), "%s/%s", argv[d], de->d_name);
            f = fopen(path, "rb");
            if (f == NULL) { continue; }
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            b = malloc((size_t)n + 16);
            if (n >= 0x30 && fread(b, 1, (size_t)n, f) == (size_t)n) {
                const char *dn = strrchr(argv[d], '/') ? strrchr(argv[d], '/') + 1 : argv[d];
                long idx = strtol(de->d_name, NULL, 10);
                long id = strcmp(dn, "pzs3us1") == 0 ? idx + 1 : strcmp(dn, "pzs3us2") == 0 ? idx + 0xD48 : -idx;
                sFileId = id < 0 ? 0 : id;
                sRole = NULL;
                xml(1, "<file name=\"%s/%s\" id=\"0x%lX\" size=\"%ld\">", dn, de->d_name, id < 0 ? 0 : id, n);
                if (memcmp(b + 0x20, "(c)CRI", 6) == 0) { xml(0, "<adx/>"); } /* (sound) */
                else { sStat[6] += (unsigned)scan(b, (uint32_t)n, 0); }
                xml(-1, "</file>");
            }
            free(b);
            fclose(f);
        }
        closedir(dir);
    }
    /* the same texture in several files is kept once */
    qsort(sOut, sOutN, sizeof(Out), key_cmp);
    for (i = 0; i < sOutN; i++) {
        if (kept && sOut[kept - 1].key == sOut[i].key) { sStat[5]++; continue; }
        sOut[kept++] = sOut[i];
    }
    f = fopen(argv[1], "wb");
    if (f == NULL) { perror(argv[1]); return 1; }
    fwrite("BT3T", 1, 4, f);
    fwrite(&kept, 4, 1, f);
    base = 8 + kept * 20;
    for (i = 0; i < kept; i++) { /* the data of the ones kept, in key order */
        uint8_t rec[20] = {0};
        uint32_t off = base;
        memcpy(rec, &sOut[i].key, 8);
        memcpy(rec + 12, &sOut[i].w, 2);
        memcpy(rec + 14, &sOut[i].h, 2);
        rec[16] = sOut[i].bits;
        rec[17] = sOut[i].psm;
        memcpy(rec + 8, &off, 4);
        fwrite(rec, 1, 20, f);
        base += (uint32_t)sOut[i].w * sOut[i].h * sOut[i].bits / 8;
    }
    for (i = 0; i < kept; i++) {
        uint32_t bytes = (uint32_t)sOut[i].w * sOut[i].h * sOut[i].bits / 8;
        uint8_t *buf = malloc(bytes);
        fseek(sData, sOut[i].offset, SEEK_SET);
        if (fread(buf, 1, bytes, sData) != bytes) { fprintf(stderr, "dc_tex: short read\n"); return 1; }
        fwrite(buf, 1, bytes, f);
        free(buf);
    }
    fclose(f);
    fclose(sData);
    remove(tmp);
    if (sXml != NULL) {
        fprintf(sXml, "</disc>\n");
        fclose(sXml);
        printf("dc_tex: %u GS image transfers in parts that are not texture tables\n", sUnknownUploads);
    }
    printf("dc_tex: %u files with textures (%u BPE parts unpacked); %u textures: %u 4-bit, %u 8-bit, %u 32-bit (the palettes kept as entries of their own); "
           "%u palettes; %u packets not understood; %u duplicates; %u in %s (%u KB)\n",
           sStat[6], sStat[7], sStat[0], sStat[1], sStat[2], sStat[3], sStat[8], sStat[4], sStat[5], kept, argv[1], base >> 10);
    return 0;
}

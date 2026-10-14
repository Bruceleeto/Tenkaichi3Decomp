/*
 * The Dreamcast's textures, converted from the disc's files ahead of time (make dc-assets): every texture file the
 * game uploads (TexFile, include/sys/gfxm_b_b.h), wherever it is (alone, inside a resource file's offset table,
 * BPE-packed), its 4-bit and 8-bit pixels as the PVR takes them: indices in twiddled order (PAL4: two to a byte,
 * the first in the low nibble; PAL8 or a VQ index map: one to a byte). The game uploads most of them as 32-bit
 * pixels, so each upload packet goes through GS memory as the GS would store it and is read back in the texture's
 * own format. Palettes are left to the game: it changes them while it runs.
 *
 * One pack, keyed by a hash of the upload's image data (the first width x height pixels of it, as the GS got them:
 * what port/src/gs/gs_core.c keeps of every 32-bit upload, Gs_SrcKey), so a texture is found however the game
 * loaded its file:
 *     "BT3T" u32 count, then count x {u64 key; u32 offset; u16 w, h; u8 bits, 0, 0, 0} sorted by key, then data
 * Run it again whenever this changes; nothing else depends on its output.
 *
 *     dc_tex OUT.pak DIR...
 */
#include <dirent.h>
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
static uint64_t sKey; /* upload(): the hash of the image data */
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
    uint8_t bits;
} Out;
static Out *sOut;
static uint32_t sOutN, sOutCap;
static FILE *sData;
static uint32_t sDataBytes;
static unsigned sStat[8]; /* texture entries, 4-bit, 8-bit, other formats, bad packets, duplicates, files with textures, BPE parts */

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
    if (sOutN == sOutCap) {
        sOutCap = sOutCap ? sOutCap * 2 : 4096;
        sOut = realloc(sOut, sOutCap * sizeof(Out));
    }
    sOut[sOutN].key = key;
    sOut[sOutN].offset = sDataBytes;
    sOut[sOutN].w = (uint16_t)w;
    sOut[sOutN].h = (uint16_t)h;
    sOut[sOutN].bits = (uint8_t)bits;
    sOutN++;
    fwrite(buf, 1, bytes, sData);
    sDataBytes += bytes;
    free(buf);
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
    for (i = 0; i < count; i++) {
        const uint8_t *e = b + ent + i * 0x40;
        uint32_t pix = rd32(e) * 4, size = rd32(e + 8), psm;
        uint64_t t0;
        memcpy(&t0, e + 0x30, 8);
        psm = (t0 >> 20) & 63;
        if (pix == 0 || size == 0) { continue; }
        sStat[0]++;
        if (psm != 0x13 && psm != 0x14) { sStat[3]++; continue; }
        memset(sVram, 0, sizeof(sVram));
        if (!upload(b + pix, size, rd32(e + 0x18), 0, 1)) { sStat[4]++; continue; }
        sStat[psm == 0x14 ? 1 : 2]++;
        if (sDump != NULL && sDumped < 40 && sStat[psm == 0x14 ? 1 : 2] % 997 == 1) {
            dump(e, b, n, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
        }
        emit(sKey, psm, (t0 >> 14) & 63, 1u << ((t0 >> 26) & 15), 1u << ((t0 >> 30) & 15));
    }
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
                if (c != left[c]) { right[c] = *in++; }
            }
            if (c == 256) { break; }
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
static int search(const uint8_t *b, uint32_t n) {
    uint32_t h;
    int found = 0;
    for (h = 0; h + 0x20 <= n; h += 4) {
        uint32_t count = rd32(b + h), ent = rd32(b + h + 4) * 4;
        if (count == 0 || count > 1024 || ent < 0x10 || ent > n || (n - ent) / 0x40 < count) { continue; }
        found |= texfile(b, n, h);
    }
    return found;
}

static int scan(const uint8_t *b, uint32_t n, int depth) {
    uint32_t count, i, found = 0;
    if (n < 16 || depth > 4) { return 0; }
    if (texfile(b, n, 0)) { return 1; }
    if (rd32(b + 4) <= n - 8 && rd32(b + 4) + 8 + 16 >= n && rd32(b) >= rd32(b + 4) && rd32(b) < (64u << 20)) {
        uint32_t size;
        uint8_t *raw = bpe(b, n, &size);
        if (raw != NULL) {
            sStat[7]++;
            found = (uint32_t)scan(raw, size, depth + 1);
            free(raw);
            return (int)found;
        }
    }
    count = rd32(b);
    if (count == 0 || count > 0x10000 || 4 + count * 4 > n) { return search(b, n); }
    for (i = 0; i < count; i++) {
        uint32_t o = rd32(b + 4 + i * 4);
        if (o != 0 && (o < 4 + count * 4 || o > n || o % 4)) { return search(b, n); } /* (one at the end: an empty part) */
    }
    for (i = 0; i < count; i++) {
        uint32_t o = rd32(b + 4 + i * 4), end = n, j;
        if (o == 0 || o == n) { continue; }
        for (j = 0; j < count; j++) { /* the part runs to the next part */
            uint32_t q = rd32(b + 4 + j * 4);
            if (q > o && q < end) { end = q; }
        }
        if (!scan(b + o, end - o, depth + 1) && !texfile(b, n, o)) { continue; } /* (counting from the part, or the file) */
        found = 1;
    }
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
            if (n >= 0x30 && fread(b, 1, (size_t)n, f) == (size_t)n && memcmp(b + 0x20, "(c)CRI", 6) != 0) { /* (not ADX sound) */
                sStat[6] += (unsigned)scan(b, (uint32_t)n, 0);
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
    printf("dc_tex: %u files with textures (%u BPE parts unpacked); %u textures: %u 4-bit, %u 8-bit, %u 32-bit (the palettes kept as entries of their own: left), "
           "%u packets not understood; %u duplicates; %u in %s (%u KB)\n",
           sStat[6], sStat[7], sStat[0], sStat[1], sStat[2], sStat[3], sStat[4], sStat[5], kept, argv[1], base >> 10);
    return 0;
}

/*
 * A file of the disc with its 4-bit and 8-bit textures' pixels left out, for the Dreamcast (make dc-assets): the
 * PVR draws those from tex.pak (port/tools/dc_tex.c), and of the pixels the game holds it only reads the hash tex.pak
 * is keyed by (port/src/gs/gs_pvr.c pkt_key). Each such texture's upload packet becomes a stub of 32 bytes (up to
 * 47, to keep what follows where it was within 16 bytes) that holds that hash:
 *     "DCKY" u32 1, u64 key, u64 key, u32 width | height << 16, u32 0
 * (the key twice: gs_pvr.c pkt_sample reads the middle and the end of a packet). The file's parts and each texture
 * file's packets close up, and their offsets with them: the game's buffer for the file is the file's size.
 *
 * Only a file that is a table of parts (u32 count, then count byte offsets, their low two bits flags), each either
 * a texture file counting from itself or something else, moved as it is. A texture is left as it is unless tex.pak
 * has it as nat_get takes it (its key, its size, 4 or 8 bits, 8-bit no more than 512 a side): the game would
 * upload it the old way, and a stub is no picture. Palettes, and the 32-, 24- and 16-bit ones (a palette can be
 * kept as one of those, Port_PalNative), are kept.
 *
 *     dc_strip TEX.PAK IN OUT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_SHAPE(key, t0) ((key) ^ ((uint64_t)(((t0) >> 14) & 0xFFFFF) + 1) * 0xD6E8FEB86659FD93ull) /* (as gs_pvr.c) */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* tex.pak's index: count x {u64 key; u32 offset; u16 w, h; u8 bits, psm, 0, 0}, sorted by key */
static uint8_t *sPak;
static uint32_t sPakN;
static int pak_find(uint64_t key, uint32_t *w, uint32_t *h, uint32_t *bits) {
    uint32_t lo = 0, hi = sPakN;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const uint8_t *q = sPak + mid * 20;
        uint64_t k;
        memcpy(&k, q, 8);
        if (k == key) {
            *w = q[12] | q[13] << 8;
            *h = q[14] | q[15] << 8;
            *bits = q[16];
            return 1;
        }
        if (k < key) { lo = mid + 1; } else { hi = mid; }
    }
    return 0;
}

/* gs_pvr.c pkt_key (pal 0): the hash of the upload's image data, its first width x height pixels; and the width
   and height. 0 = not such a packet. */
static int pkt_key(const uint8_t *p, uint32_t size, uint32_t blt, uint64_t *key, uint32_t *w, uint32_t *h) {
    uint32_t dpsm = (blt >> 8) & 63, bits = dpsm == 0 ? 32 : dpsm == 1 ? 24 : dpsm == 2 || dpsm == 0xA ? 16 : dpsm == 0x13 ? 8 : 4;
    uint32_t rw = 0, rh = 0, at = 0, want = 0, images = 0, i;
    uint64_t hh = 0xCBF29CE484222325ull;
    if (size >= 16 && (p[15] & 0x7F) == 0x50) { at = 16; } /* VIF DIRECT */
    while (at + 16 <= size) {
        uint64_t tag = (uint64_t)rd32(p + at) | (uint64_t)rd32(p + at + 4) << 32;
        uint32_t nloop = tag & 0x7FFF, flg = (tag >> 58) & 3, nreg = (tag >> 60) & 15, n;
        at += 16;
        if (flg == 0 && nreg == 1 && (p[at - 8] & 15) == 0xE) { /* A+D: TRXREG */
            for (i = 0; i < nloop && at + 16 <= size; i++, at += 16) {
                if (p[at + 8] == 0x52) { rw = rd32(p + at) & 0xFFF; rh = rd32(p + at + 4) & 0xFFF; }
            }
        } else if (flg == 2) { /* IMAGE */
            if (images++ == 0) { want = rw * rh * bits / 8; }
            n = nloop * 16 < size - at ? nloop * 16 : size - at;
            for (i = 0; i < n && want != 0; i++, want--) { hh = (hh ^ p[at + i]) * 0x100000001B3ull; }
            at += nloop * 16;
        } else {
            break;
        }
    }
    *key = hh;
    *w = rw;
    *h = rh;
    return images != 0 && rw != 0;
}

static unsigned sStubbed, sKept, sParts, sTexParts;

/* A packet made a stub: at, its size, the bytes it gives up (0 = left as it is), the entry whose packet it is. */
typedef struct Range { uint32_t at, size, cut, e; } Range;
static int range_cmp(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}
/* Where offset x is once the ranges before it gave up their bytes. */
static uint32_t moved(const Range *r, uint32_t nr, uint32_t x) {
    uint32_t j, d = 0;
    for (j = 0; j < nr && r[j].at < x; j++) { d += r[j].cut; }
    return x - d;
}

/* Whether b (n bytes) is a texture file counting from itself, every entry's packets in it. */
static int is_texfile(const uint8_t *b, uint32_t n) {
    uint32_t count, ent, i;
    if (n < 0x20) { return 0; }
    count = rd32(b);
    ent = rd32(b + 4) * 4;
    if (count == 0 || count > 1024 || ent < 0x10 || ent > n || (n - ent) / 0x40 < count) { return 0; }
    for (i = 0; i < count; i++) {
        const uint8_t *e = b + ent + i * 0x40;
        uint32_t pix = rd32(e) * 4, size = rd32(e + 8), clut = rd32(e + 4) * 4, csize = rd32(e + 12);
        uint64_t t0;
        memcpy(&t0, e + 0x30, 8);
        if (pix > n || size > n - pix || clut > n || csize > n - clut || ((t0 >> 26) & 15) > 10 || ((t0 >> 30) & 15) > 10) { return 0; }
    }
    return 1;
}

/* A texture file (is_texfile) into out (room for n): its stubbed packets closed up. Its new size. */
static uint32_t texfile(const uint8_t *b, uint32_t n, uint8_t *out) {
    uint32_t count = rd32(b), ent = rd32(b + 4) * 4, i, j, nr = 0, at, o, keep;
    Range *r = calloc(count, sizeof(Range));
    for (i = 0; i < count; i++) {
        const uint8_t *e = b + ent + i * 0x40;
        uint32_t pix = rd32(e) * 4, size = rd32(e + 8), psm, w, h, bits, rw, rh, tw, th;
        uint64_t t0, key;
        memcpy(&t0, e + 0x30, 8);
        psm = (t0 >> 20) & 63;
        if (pix == 0 || size < 48 || pix % 16 != 0 || (psm != 0x13 && psm != 0x14)) { continue; }
        if (!pkt_key(b + pix, size, rd32(e + 0x18), &key, &rw, &rh)) { continue; }
        tw = 1u << ((t0 >> 26) & 15);
        th = 1u << ((t0 >> 30) & 15);
        if (!pak_find(KEY_SHAPE(key, t0), &w, &h, &bits) || w != (tw < 8 ? 8 : tw) || h != (th < 8 ? 8 : th) ||
            bits != (psm == 0x14 ? 4u : 8u) || (bits == 8 && (w > 512 || h > 512))) { /* (as nat_get takes it) */
            sKept++;
            continue;
        }
        for (j = 0; j < nr && r[j].at != pix; j++) {}
        if (j < nr) { /* (two entries of one packet: one stub, if it is all of it for both) */
            if (r[j].size != size || rd32(b + r[j].e + 0x18) != rd32(e + 0x18)) { r[j].cut = 0; }
            continue;
        }
        r[nr].at = pix;
        r[nr].size = size;
        r[nr].cut = (size - 32) & ~15u;
        r[nr].e = ent + i * 0x40;
        nr++;
    }
    qsort(r, nr, sizeof(Range), range_cmp);
    /* a range keeps its bytes if it overlaps another, the header or the entries, or an offset points into it */
    for (j = 0; j < nr; j++) {
        uint32_t lo = r[j].at, hi = r[j].at + r[j].size;
        if (j + 1 < nr && r[j + 1].at < hi) { r[j].cut = r[j + 1].cut = 0; }
        if (lo < 0x20 || (lo < ent + count * 0x40 && hi > ent)) { r[j].cut = 0; }
        for (i = 0; i < count; i++) {
            const uint8_t *e = b + ent + i * 0x40;
            uint32_t p = rd32(e) * 4, c = rd32(e + 4) * 4;
            if ((p > lo && p < hi) || (c != 0 && c >= lo && c < hi)) { r[j].cut = 0; }
        }
    }
    for (j = 0, keep = 0; j < nr; j++) { /* (the ones left as they are dropped: nothing moves for them) */
        if (r[j].cut != 0) { r[keep++] = r[j]; }
    }
    nr = keep;
    /* the file again: the bytes outside the ranges, a stub at each */
    for (at = 0, o = 0, j = 0; at < n;) {
        if (j < nr && at == r[j].at) {
            uint32_t stub = r[j].size - r[j].cut, rw, rh;
            uint64_t key;
            pkt_key(b + at, r[j].size, rd32(b + r[j].e + 0x18), &key, &rw, &rh);
            memset(out + o, 0, stub);
            memcpy(out + o, "DCKY", 4);
            wr32(out + o + 4, 1);
            memcpy(out + o + 8, &key, 8);
            memcpy(out + o + 16, &key, 8);
            wr32(out + o + 24, rw | rh << 16);
            sStubbed++;
            o += stub;
            at += r[j].size;
            j++;
            continue;
        }
        out[o++] = b[at++];
    }
    /* the offsets: each less what the ranges before it gave up; a stub's entries their packet's new size */
    {
        uint8_t *e = out + moved(r, nr, ent);
        wr32(out + 4, moved(r, nr, ent) / 4);
        for (i = 0; i < count; i++, e += 0x40) {
            uint32_t p = rd32(e) * 4, c = rd32(e + 4) * 4;
            if (p != 0) {
                for (j = 0; j < nr && r[j].at != p; j++) {}
                if (j < nr) { wr32(e + 8, r[j].size - r[j].cut); }
                wr32(e, moved(r, nr, p) / 4);
            }
            if (c != 0) { wr32(e + 4, moved(r, nr, c) / 4); }
        }
    }
    free(r);
    return o;
}

static int u32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    FILE *f;
    uint8_t *b, *out, hdr[8];
    uint32_t n, count, i, j, nu = 0, o, *starts, *news;
    long size;

    if (argc != 4) {
        fprintf(stderr, "usage: dc_strip TEX.PAK IN OUT\n");
        return 2;
    }
    if ((f = fopen(argv[1], "rb")) == NULL || fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "BT3T", 4) != 0) {
        fprintf(stderr, "dc_strip: %s: not a tex.pak\n", argv[1]);
        return 1;
    }
    sPakN = rd32(hdr + 4);
    sPak = malloc((size_t)sPakN * 20);
    if (fread(sPak, 20, sPakN, f) != sPakN) { fprintf(stderr, "dc_strip: %s: short\n", argv[1]); return 1; }
    fclose(f);
    if ((f = fopen(argv[2], "rb")) == NULL) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    n = (uint32_t)size;
    b = malloc(n + 64);
    if (fread(b, 1, n, f) != n) { perror(argv[2]); return 1; }
    fclose(f);

    /* the table of parts: each part runs to the next start (or the end), in the order they lie */
    count = n >= 8 ? rd32(b) : 0;
    if (count == 0 || count > 0x10000 || 4 + count * 4 > n) { fprintf(stderr, "dc_strip: %s: not a table of parts\n", argv[2]); return 1; }
    starts = malloc((count + 1) * 4);
    news = malloc((count + 1) * 4);
    for (i = 0; i < count; i++) {
        uint32_t s = rd32(b + 4 + i * 4) & ~3u;
        if (s == 0) { continue; }
        if (s < 4 + count * 4 || s > n || s % 16 != 0) { fprintf(stderr, "dc_strip: %s: part %u at 0x%X\n", argv[2], i, s); return 1; }
        starts[nu++] = s;
    }
    starts[nu++] = n;
    qsort(starts, nu, 4, u32_cmp);
    for (i = 1, j = 1; i < nu; i++) {
        if (starts[i] != starts[j - 1]) { starts[j++] = starts[i]; }
    }
    nu = j;
    out = calloc(1, n + 64 * nu);
    memcpy(out, b, starts[0]); /* (the table, filled in below, and what pads it) */
    for (i = 0, o = starts[0]; i + 1 < nu; i++) {
        uint32_t at = starts[i], len = starts[i + 1] - at, made;
        news[i] = o;
        sParts++;
        if (is_texfile(b + at, len)) {
            made = texfile(b + at, len, out + o);
            sTexParts++;
        } else {
            memcpy(out + o, b + at, len);
            made = len;
        }
        o += (made + 63) & ~63u; /* (the parts on 64 bytes, as the disc's) */
    }
    news[nu - 1] = o;
    for (i = 0; i < count; i++) {
        uint32_t w = rd32(b + 4 + i * 4), s = w & ~3u;
        if (s == 0) { continue; }
        for (j = 0; starts[j] != s; j++) {}
        wr32(out + 4 + i * 4, news[j] | (w & 3));
    }
    if ((f = fopen(argv[3], "wb")) == NULL || fwrite(out, 1, o, f) != o || fclose(f) != 0) { perror(argv[3]); return 1; }
    printf("dc_strip: %s: %u parts (%u texture files), %u packets stubbed, %u not in tex.pak kept; %u -> %u KB\n", argv[2], sParts,
           sTexParts, sStubbed, sKept, n >> 10, o >> 10);
    return 0;
}

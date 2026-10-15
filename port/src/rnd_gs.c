/*
 * The GS state the game sets, followed as the renderer's own (include/port/rnd.h gRndCtx), and the ordering table's
 * packets as polygons.
 *
 * The game sets the GS's state with packets of register writes and draws under it. Each packet it closes in the
 * display list (src/sys/dma.c Dma_AddData, Dma_EndDirect) is read here as it is made, in the order the GS will
 * see it: its register writes become the state of the two contexts (gRndCtx), so a draw handed over as a record
 * takes the state the GS would have drawn it with. Nothing is read from the GS. (Not followed: packets the list
 * names by address (REF, chains: the models' VU1 programs) and those of Dma_Alloc. BT3_RND_CHECK reports a record
 * whose state differs from the GS's.)
 *
 * The effects (src/battle/eft_*.c) queue GS packets in the ordering table (src/sys/gfx_ot.c), each a GIF tag and
 * the vertices of a few triangles. While the table is sent (Ot_Send) each one is read here: a draw whose state is
 * known is handed to the renderer as polygons (Port_Poly), in its place among the packets that stay; any other
 * packet stays, and its register writes are followed. What a draw taken would have left in the GS (TEX0 and its
 * palette load, CLAMP, TEX1, PRIM) is written as A+D before the next packet that stays, so what is still a packet
 * draws as it did. An effect that queues its draw as a record instead (src/sys/gfx_ot.c Ot_NewRec) has it read
 * through the same steps, corner by corner (Rnd_OtRec), so it is drawn exactly as its packet would have been.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../../include/port/rnd.h"

unsigned gRndOtStat[3];
uint32_t gRndCtxBad[2];       /* RND_BAD_: what of a context's state a record cannot say */

static uint64_t sTex0[2];     /* TEX0 of each context */
static uint32_t sTexH[2], sPalH[2]; /* the handles of the texture it names, as a record of the table gave them (0: none) */
static uint64_t sXyOff[2] = {0x7200ull << 32 | 0x7000, 0x7200ull << 32 | 0x7000}; /* XYOFFSET */
static uint64_t sPrim;        /* PRIM */
static uint64_t sPrmodeCont = 1, sPrmode; /* PRMODECONT, PRMODE: with AC 0 a primitive's attributes are PRMODE's */
static int sInOt;             /* the table is being sent: its packets are read by Rnd_OtTake, not again when closed */

/* The registers a draw taken set that the GS does not have yet (Rnd_OtState): the values as written. */
enum { D_TEX0, D_CLAMP, D_TEX1, D_N };
static const uint8_t sDirtyAddr[D_N] = {0x06, 0x08, 0x14};
static uint64_t sDirtyVal[2][D_N], sDirtyPrim;
static uint32_t sDirty; /* bit ctx * D_N + d; 31: PRIM */

static void dirty(int d, int c, uint64_t v, int taken) {
    sDirtyVal[c][d] = v;
    sDirty = taken ? sDirty | 1u << (c * D_N + d) : sDirty & ~(1u << (c * D_N + d));
}

/* A GS register written (with the address of context 1's register + 1 for context 2's): into the state. taken:
   by a draw handed over (the GS does not get it: Rnd_OtState). 0 = one not followed. */
static int gs_write(uint32_t addr, uint64_t v, int taken) {
    static const uint8_t atst[8] = {RND_ATEST_NEVER, RND_ATEST_ALWAYS, 2, 3, 4, 5, 6, 7}; /* the GS's ATST -> RND_ATEST_ */
    int c = 0;
    RndState *s;

    if (addr == 0x00) {
        sPrim = v;
        sDirtyPrim = v;
        sDirty = taken ? sDirty | 1u << 31 : sDirty & ~(1u << 31);
        return 1;
    }
    switch (addr) { /* the registers of the two contexts */
    case 0x07: case 0x09: case 0x15: case 0x17: case 0x19: case 0x41: case 0x43: case 0x48: case 0x4B: case 0x4D: case 0x4F:
        c = 1;
        addr--;
        break;
    }
    s = &gRndCtx[c];
    switch (addr) {
    case 0x06: /* TEX0 */
        sTex0[c] = v;
        sTexH[c] = sPalH[c] = 0; /* (Rnd_OtRec gives its own) */
        dirty(D_TEX0, c, v, taken);
        return 1;
    case 0x16: /* TEX2: TEX0's format and palette */
        sTex0[c] = (sTex0[c] & ~0xFFFFFFE003F00000ull) | (v & 0xFFFFFFE003F00000ull);
        sTexH[c] = sPalH[c] = 0;
        return 1;
    case 0x08: /* CLAMP: repeated, clamped, or a region (not said) */
        v &= 15;
        s->flags = (uint16_t)(v == 0 ? s->flags | RND_REPEAT : s->flags & ~RND_REPEAT);
        gRndCtxBad[c] = v == 0 || v == 5 ? gRndCtxBad[c] & ~RND_BAD_CLAMP : gRndCtxBad[c] | RND_BAD_CLAMP;
        dirty(D_CLAMP, c, v, taken);
        return 1;
    case 0x14: /* TEX1: filtered when magnified */
        s->flags = (uint16_t)((v >> 5) & 1 ? s->flags | RND_SMOOTH : s->flags & ~RND_SMOOTH);
        dirty(D_TEX1, c, v, taken);
        return 1;
    case 0x18: /* XYOFFSET */
        sXyOff[c] = v;
        return 1;
    case 0x40: /* SCISSOR */
        s->scissor[0] = (int16_t)(v & 0x7FF);
        s->scissor[2] = (int16_t)((v >> 16) & 0x7FF);
        s->scissor[1] = (int16_t)((v >> 32) & 0x7FF);
        s->scissor[3] = (int16_t)((v >> 48) & 0x7FF);
        return 1;
    case 0x42: { /* ALPHA: (A - B) * C + D as one of RND_BLEND_, by the source's alpha or the destination's */
        uint64_t b = v & 0xFF & ~0x30ull;
        uint32_t k = (uint32_t)(v >> 4) & 3;
        s->blend = b == 0x44 ? RND_BLEND_NORMAL : b == 0x48 ? RND_BLEND_ADD : b == 0x42 ? RND_BLEND_SUB : b == 0x49 ? RND_BLEND_DEST : RND_BLEND_NORMAL;
        s->flags = (uint16_t)(k == 1 ? s->flags | RND_DEST_ALPHA : s->flags & ~RND_DEST_ALPHA);
        s->flags &= (uint16_t)~(RND_ONE | RND_FIX);
        if (k == 2 && ((v >> 32) & 0xFF) == 0x80) { /* by 1: (Cs - Cd) * 1 + Cd is the source as it is (the fighters' lit draw) */
            s->blend = b == 0x44 ? RND_BLEND_NONE : s->blend;
            s->flags |= b == 0x44 ? 0 : RND_ONE;
            k = 0;
        } else if (k == 2) { /* by another constant (the fighters' flat shadow: normal by 0x46) */
            s->flags |= RND_ONE | RND_FIX;
            s->fix = (uint8_t)(v >> 32);
            k = 0;
        }
        gRndCtxBad[c] = (b == 0x44 || b == 0x48 || b == 0x42 || b == 0x49) && k <= 1 ? gRndCtxBad[c] & ~RND_BAD_BLEND : gRndCtxBad[c] | RND_BAD_BLEND;
        return 1;
    }
    case 0x47: /* TEST */
        s->atest = v & 1 ? atst[(v >> 1) & 7] : RND_ATEST_ALWAYS;
        s->aref = v & 1 ? (uint8_t)(v >> 4) : 0;
        s->afail = v & 1 ? (uint8_t)((v >> 12) & 3) : 0;
        s->mask = (v >> 14) & 1 ? ((v >> 15) & 1 ? RND_IF_MARKED : RND_IF_UNMARKED) : RND_MASK_NONE;
        s->ztest = !((v >> 16) & 1) || ((v >> 17) & 3) == 1 ? RND_Z_ALWAYS : ((v >> 17) & 3) == 2 ? RND_Z_GEQUAL : RND_Z_GREATER;
        gRndCtxBad[c] = ((v >> 16) & 1) && ((v >> 17) & 3) == 0 ? gRndCtxBad[c] | RND_BAD_TEST : gRndCtxBad[c] & ~RND_BAD_TEST; /* (never) */
        return 1;
    case 0x4A: /* FBA */
        s->flags = (uint16_t)(v & 1 ? s->flags | RND_MARK : s->flags & ~RND_MARK);
        return 1;
    case 0x4C: { /* FRAME: the buffer, and which of its channels are written */
        uint64_t m = v >> 32;
        s->fbp = (uint32_t)v & 0x1FF;
        s->flags = (uint16_t)((s->flags & ~(RND_NO_COLOUR | RND_NO_ALPHA)) | (m == 0x00FFFFFF ? RND_NO_COLOUR : 0) | (m == 0xFF000000u ? RND_NO_ALPHA : 0));
        gRndCtxBad[c] = m == 0 || m == 0x00FFFFFF || m == 0xFF000000u ? gRndCtxBad[c] & ~RND_BAD_FRAME : gRndCtxBad[c] | RND_BAD_FRAME;
        return 1;
    }
    case 0x4E: /* ZBUF: depth written or not */
        s->flags = (uint16_t)((v >> 32) & 1 ? s->flags & ~RND_ZWRITE : s->flags | RND_ZWRITE);
        return 1;
    case 0x1A: /* PRMODECONT */
        sPrmodeCont = v;
        return 1;
    case 0x1B: /* PRMODE */
        sPrmode = v;
        return 1;
    }
    /* what leaves nothing a record reads: TEXFLUSH, COLCLAMP, DTHE, PABE, TEXA, TEXCLUT, FOGCOL, the transfers,
       FINISH, SIGNAL, LABEL, the port's markers (0x7B..0x7F: port/src/gs/gs_core.c), and the vertex registers by
       address (draws, not state) */
    return addr <= 0x05 || addr == 0x0A || addr == 0x0C || addr == 0x0D || addr == 0x0F || addr == 0x1C || addr == 0x3B ||
           addr == 0x3D || addr == 0x3F || addr == 0x44 || addr == 0x45 || addr == 0x46 || addr == 0x49 || (addr >= 0x50 && addr <= 0x54) ||
           (addr >= 0x60 && addr <= 0x62) || (addr >= 0x7B && addr <= 0x7F);
}

/* How a packet is read: whether it can be taken, its polygons to the queue, or only its register writes. */
enum { CHECK, DRAW, FOLLOW };

/* The vertex queue of the primitive being drawn. */
static RndVtx sV[3];
static int sVn;
static uint32_t sRgba;
static float sS, sT, sQ = 1.0f;
static int sU, sVv;

/* A vertex (kick: one that draws): the polygon it completes to the queue; 0 = one that cannot be drawn. */
static int vertex(uint32_t x, uint32_t y, uint32_t z, int kick, int mode) {
    int type = (int)(sPrim & 7), c = (int)(sPrim >> 9) & 1, fst = (int)(sPrim >> 8) & 1, tme = (int)(sPrim >> 4) & 1;
    int need = type == 6 || type <= 2 ? 2 : 3; /* (the vertices a primitive takes) */
    uint64_t t0 = sTex0[c];
    RndVtx v;
    RndPoly p;

    if (mode == FOLLOW) {
        return 1;
    }
    if (type == 0 || type > 6 || ((sPrim >> 5) & 1)) { /* (points; fog) */
        return 0;
    }
    v.x = (float)((int32_t)(x & 0xFFFF) - (int32_t)(sXyOff[c] & 0xFFFF)) / 16.0f; /* (sixteenths of a pixel, from XYOFFSET) */
    v.y = (float)((int32_t)(y & 0xFFFF) - (int32_t)((sXyOff[c] >> 32) & 0xFFFF)) / 16.0f;
    v.z = z;
    v.rgba = sRgba;
    if (tme && fst) {
        v.s = (float)sU / 16.0f / (float)(1u << ((t0 >> 26) & 15));
        v.t = (float)sVv / 16.0f / (float)(1u << ((t0 >> 30) & 15));
        v.q = 1.0f;
    } else {
        v.s = sS;
        v.t = sT;
        v.q = sQ;
    }
    if (sVn == need) { /* the queue full: a fan keeps its first, the others their last two (a line strip its last) */
        if (type == 5) {
            sV[1] = sV[2];
        } else {
            sV[0] = sV[1];
            sV[1] = sV[2];
        }
        sVn--;
    }
    sV[sVn++] = v;
    if (!kick || sVn < need) {
        return 1;
    }
    if (gRndCtxBad[c] != 0) {
        return 0;
    }
    memset(&p, 0, sizeof(p));
    p.st = gRndCtx[c];
    p.st.flags = (uint16_t)((p.st.flags & ~RND_GOURAUD) | ((sPrim >> 3) & 1 ? RND_GOURAUD : 0));
    if (!((sPrim >> 6) & 1)) {
        p.st.blend = RND_BLEND_NONE;
    }
    if (type == 6) { /* a sprite: two corners, the second's colour and depth; s, t not divided per pixel */
        const RndVtx *a = &sV[0], *b = &sV[1];
        float q = b->q != 0.0f ? b->q : 1.0f;
        int i;
        for (i = 0; i < 4; i++) {
            p.v[i] = *b;
            p.v[i].x = i & 1 ? b->x : a->x;
            p.v[i].y = i & 2 ? b->y : a->y;
            p.v[i].s = (i & 1 ? b->s : a->s) / q;
            p.v[i].t = (i & 2 ? b->t : a->t) / q;
            p.v[i].q = 1.0f;
        }
        p.n = 4;
        p.st.flags &= ~RND_GOURAUD;
    } else if (type <= 2) { /* a line: a quad one pixel wide along it, each end's colour at its two corners */
        float dx = sV[1].x - sV[0].x, dy = sV[1].y - sV[0].y, len = sqrtf(dx * dx + dy * dy);
        int i;
        if (type == 1) {
            sVn = 0;
        }
        if (len == 0.0f) {
            return 1;
        }
        dx *= 0.5f / len;
        dy *= 0.5f / len;
        for (i = 0; i < 4; i++) {
            p.v[i] = sV[i & 1];
            p.v[i].x += i & 2 ? dy : -dy;
            p.v[i].y += i & 2 ? -dx : dx;
        }
        p.n = 4;
    } else {
        memcpy(p.v, sV, sizeof(sV));
        p.n = 3;
    }
    if (type == 3 || type == 6) {
        sVn = 0;
    }
    p.gsTex0 = tme ? t0 : 0;
    p.tex = tme ? sTexH[c] : 0;
    p.pal = tme ? sPalH[c] : 0;
    p.gsCtx = (uint32_t)c;
    if (mode == DRAW) {
        Port_Poly(&p);
    }
    return 1;
}

/* Reads one packet's GIF data. Returns the vertices that draw, or -1: one it cannot read (or draw). */
static int walk(const uint64_t *d, unsigned qwc, int mode) {
    const uint64_t *end = d + qwc * 2;
    int kicks = 0, ok = 1, taken = mode == DRAW;

    sVn = 0;
    while (d + 2 <= end) {
        uint64_t tag = d[0], regs = d[1];
        unsigned nloop = (unsigned)(tag & 0x7FFF), flg = (unsigned)(tag >> 58) & 3, nreg = (unsigned)(tag >> 60) & 15, i, j;
        if (nreg == 0) { nreg = 16; }
        d += 2;
        if (flg >= 2) { /* IMAGE: an upload's data */
            d += nloop * 2;
            ok = 0;
            continue;
        }
        if ((tag >> 46) & 1) { /* PRE: PRIM from the tag */
            gs_write(0, (tag >> 47) & 0x7FF, taken);
        }
        for (i = 0; i < nloop; i++) {
            for (j = 0; j < nreg; j++) {
                uint32_t r = (uint32_t)(regs >> (j * 4)) & 15;
                uint64_t lo, hi = 0;
                int k = -1;
                if (d + (flg == 1 ? 1 : 2) > end) { return -1; }
                lo = d[0];
                if (flg == 1) { /* REGLIST: a doubleword each */
                    d++;
                    switch (r) {
                    case 1: sRgba = (uint32_t)lo; memcpy(&sQ, (const uint8_t *)&lo + 4, 4); break;
                    case 2: memcpy(&sS, &lo, 4); memcpy(&sT, (const uint8_t *)&lo + 4, 4); break;
                    case 3: sU = (int)(lo & 0x3FFF); sVv = (int)((lo >> 16) & 0x3FFF); break;
                    case 4: case 12: k = vertex((uint32_t)lo & 0xFFFF, (uint32_t)(lo >> 16) & 0xFFFF, (uint32_t)(lo >> 32) & 0xFFFFFF, r == 4, mode); break;
                    case 5: case 13: k = vertex((uint32_t)lo & 0xFFFF, (uint32_t)(lo >> 16) & 0xFFFF, (uint32_t)(lo >> 32), r == 5, mode); break;
                    case 0: case 6: case 7: case 8: case 9: gs_write(r, lo, taken); break;
                    case 10: case 15: break;
                    default: ok = 0; break;
                    }
                } else {        /* PACKED: a quadword each */
                    hi = d[1];
                    d += 2;
                    switch (r) {
                    case 1: sRgba = (uint32_t)(lo & 0xFF) | (uint32_t)((lo >> 32) & 0xFF) << 8 | (uint32_t)(hi & 0xFF) << 16 | (uint32_t)((hi >> 32) & 0xFF) << 24; break;
                    case 2: memcpy(&sS, &lo, 4); memcpy(&sT, (const uint8_t *)&lo + 4, 4); memcpy(&sQ, &hi, 4); break;
                    case 3: sU = (int)(lo & 0x3FFF); sVv = (int)((lo >> 32) & 0x3FFF); break;
                    case 4: k = vertex((uint32_t)lo & 0xFFFF, (uint32_t)(lo >> 32) & 0xFFFF, (uint32_t)(hi >> 4) & 0xFFFFFF, !((hi >> 47) & 1), mode); break;
                    case 5: k = vertex((uint32_t)lo & 0xFFFF, (uint32_t)(lo >> 32) & 0xFFFF, (uint32_t)hi, !((hi >> 47) & 1), mode); break;
                    case 0: gs_write(0, lo & 0x7FF, taken); break;
                    case 6: case 7: case 8: case 9: gs_write(r, lo, taken); break;
                    case 14: /* A+D */
                        if ((hi & 0xFF) == 0x00) { /* PRIM, RGBAQ, ST, UV by address: in their registers' format, as REGLIST */
                            gs_write(0, lo & 0x7FF, taken);
                        } else if ((hi & 0xFF) == 0x01) {
                            sRgba = (uint32_t)lo; memcpy(&sQ, (const uint8_t *)&lo + 4, 4);
                        } else if ((hi & 0xFF) == 0x02) {
                            memcpy(&sS, &lo, 4); memcpy(&sT, (const uint8_t *)&lo + 4, 4);
                        } else if ((hi & 0xFF) == 0x03) {
                            sU = (int)(lo & 0x3FFF); sVv = (int)((lo >> 16) & 0x3FFF);
                        } else if ((hi & 0xFF) <= 0x05 || (hi & 0xFF) == 0x0C || (hi & 0xFF) == 0x0D) { /* (vertex registers by address) */
                            ok = 0;
                        } else if (!gs_write((uint32_t)hi & 0xFF, lo, taken)) {
                            static int told;
                            ok = 0;
                            if (!told++ && getenv("BT3_GS_VERBOSE") != NULL) { fprintf(stderr, "rnd: GS register %02X written: not followed\n", (unsigned)(hi & 0xFF)); }
                        }
                        break;
                    case 10: case 15: break;
                    default: ok = 0; break;
                    }
                }
                if (k == 0) {
                    ok = 0;
                } else if (k > 0) {
                    kicks++;
                }
            }
        }
        /* (EOP ends a GIF packet, not the transfer: a tag that follows in it is read as well) */
    }
    return ok ? kicks : -1;
}

int Rnd_GsFollow(const uint32_t *pkt, unsigned bytes) {
    const uint32_t *end = pkt + bytes / 4;
    int only = 1;
    if (sInOt) {
        return 0;
    }
    while (pkt + 4 <= end) { /* DMA CNT tags, each with its VIF codes: the GIF data of a DIRECT */
        unsigned qwc = pkt[0] & 0xFFFF, id = (pkt[0] >> 28) & 7;
        if (id != 1 || pkt + 4 + qwc * 4 > end) {
            return 0;
        }
        if ((pkt[3] >> 24 & 0x7F) == 0x50 || (pkt[3] >> 24 & 0x7F) == 0x51) {
            only &= walk((const uint64_t *)(pkt + 4), qwc, FOLLOW) == 0; /* (no draw, no upload, every register followed) */
        } else {
            only = 0;
        }
        pkt += 4 + qwc * 4;
    }
    return only && pkt == end;
}

void Rnd_OtBegin(void) {
    sInOt = 1;
}

void Rnd_OtEnd(void) {
    sInOt = 0;
}

int Rnd_OtTake(const uint64_t *pkt, unsigned qwc) {
    RndState ctx[2];
    uint32_t bad[2], dirtyMask = sDirty;
    uint64_t tex0[2], xy[2], prim = sPrim, dv[2][D_N], dp = sDirtyPrim, prc = sPrmodeCont, prm = sPrmode;
    int n;

    if (!Port_TakesRecords()) {
        walk(pkt, qwc, FOLLOW);
        return 0;
    }
    memcpy(ctx, gRndCtx, sizeof(ctx));
    memcpy(bad, gRndCtxBad, sizeof(bad));
    memcpy(tex0, sTex0, sizeof(tex0));
    memcpy(xy, sXyOff, sizeof(xy));
    memcpy(dv, sDirtyVal, sizeof(dv));
    n = walk(pkt, qwc, CHECK);
    memcpy(gRndCtx, ctx, sizeof(ctx)); /* (the state as it was, for Rnd_OtDraw or the packet's own writes) */
    memcpy(gRndCtxBad, bad, sizeof(bad));
    memcpy(sTex0, tex0, sizeof(tex0));
    memcpy(sXyOff, xy, sizeof(xy));
    memcpy(sDirtyVal, dv, sizeof(dv));
    sPrim = prim;
    sPrmodeCont = prc;
    sPrmode = prm;
    sDirtyPrim = dp;
    sDirty = dirtyMask;
    if (n > 0) {
        gRndOtStat[1]++;
        return 1;
    }
    walk(pkt, qwc, FOLLOW); /* a packet that stays: what it sets */
    if (n != 0) { /* (a draw, not one that only sets state) */
        gRndOtStat[0]++;
    }
    return 0;
}

void Rnd_OtDraw(const uint64_t *pkt, unsigned qwc) {
    walk(pkt, qwc, DRAW);
}

uint64_t *Rnd_OtState(uint64_t *dst) {
    uint64_t *tag = dst;
    unsigned n = 0;
    int b;

    if (sDirty == 0) {
        return dst;
    }
    dst += 2;
    for (b = 0; b < 2 * D_N; b++) {
        if (sDirty >> b & 1) {
            dst[0] = sDirtyVal[b / D_N][b % D_N];
            dst[1] = sDirtyAddr[b % D_N] + (uint64_t)(b / D_N);
            dst += 2;
            n++;
        }
    }
    if (sDirty >> 31 & 1) {
        dst[0] = sDirtyPrim;
        dst[1] = 0;
        dst += 2;
        n++;
    }
    tag[0] = n | 1ull << 15 | 1ull << 60; /* NLOOP n, EOP, PACKED, one register: A+D */
    tag[1] = 0xE;
    sDirty = 0;
    return dst;
}

/* The PRIM a record's packet would write. */
static uint64_t rec_prim(const RndOtRec *r) {
    return (uint64_t)(r->type & 7) | (uint64_t)(r->gouraud != 0) << 3 | (uint64_t)(r->tme != 0) << 4 | (uint64_t)(r->abe != 0) << 6 |
           (uint64_t)(r->fst != 0) << 8 | (uint64_t)(r->ctx & 1) << 9;
}

int Rnd_OtRecTakes(const RndOtRec *r) {
    return Port_TakesRecords() && gRndCtxBad[r->ctx & 1] == 0 && r->type >= 1 && r->type <= 6 && r->n <= 4;
}

/* Its corners go through vertex() as the packet's would: its PRIM and TEX0 written as taken, then each corner's
   colour and Q, texture coordinates (textured) and position, each one a kick. */
void Rnd_OtRec(const RndOtRec *r) {
    int i;

    gs_write(0, rec_prim(r), 1);
    if (r->setTex0 & 1) {
        gs_write(0x06 + (r->ctx & 1), r->tex0, 1);
    }
    if (r->setTex0 & RND_OTREC_CLAMP) {
        gs_write(0x08 + (r->ctx & 1), 5, 1);
    }
    sVn = 0;
    if (r->tex != 0) { /* (given with its TEX0, which has just cleared the context's) */
        sTexH[r->ctx & 1] = r->tex;
        sPalH[r->ctx & 1] = r->pal;
    }
    for (i = 0; i < r->n; i++) {
        const RndOtVtx *v = &r->v[i];
        sRgba = v->rgba;
        sQ = v->q;
        if (r->tme && r->fst) {
            sU = (int)v->s & 0x3FFF;
            sVv = (int)v->t & 0x3FFF;
        } else if (r->tme) { /* (an untextured packet writes no ST: what the last one wrote stays) */
            sS = v->s;
            sT = v->t;
        }
        vertex(v->x, v->y, v->z & 0xFFFFFF, 1, DRAW);
    }
    if (r->setTex0 & RND_OTREC_CLAMP) {
        gs_write(0x08 + (r->ctx & 1), 0, 1);
    }
    gRndOtStat[2]++;
}

/* The packet: PRIM (and TEX0) as PACKED registers, then each corner's RGBAQ, ST (Q in it) or UV when textured,
   and XYZ2. (The Q of an untextured one or of UV is left as it was: nothing reads it.) */
uint64_t *Rnd_OtRecPacket(const RndOtRec *r, uint64_t *dst) {
    uint64_t *start = dst;
    int i;

    int tex = r->setTex0 & 1, clamp = (r->setTex0 & RND_OTREC_CLAMP) != 0;

    dst[0] = 1 | (uint64_t)(1 + tex + clamp) << 60; /* NLOOP 1, PACKED: PRIM, TEX0, CLAMP */
    dst[1] = (tex ? (uint64_t)(0x06 + (r->ctx & 1)) << 4 : 0) | (clamp ? (uint64_t)(0x08 + (r->ctx & 1)) << (4 + 4 * tex) : 0);
    dst[2] = rec_prim(r);
    dst[3] = 0;
    dst += 4;
    if (tex) {
        dst[0] = r->tex0;
        dst[1] = 0;
        dst += 2;
    }
    if (clamp) {
        dst[0] = 5;
        dst[1] = 0;
        dst += 2;
    }
    if (r->tme) {
        dst[0] = (uint64_t)r->n | 1ull << 15 | 3ull << 60; /* NLOOP n, EOP, PACKED, three registers */
        dst[1] = 0x1 | (uint64_t)(r->fst ? 0x3 : 0x2) << 4 | 0x5ull << 8;
    } else {
        dst[0] = (uint64_t)r->n | 1ull << 15 | 2ull << 60; /* two: RGBAQ, XYZ2 */
        dst[1] = 0x1 | 0x5ull << 4;
    }
    dst += 2;
    for (i = 0; i < r->n; i++) {
        const RndOtVtx *v = &r->v[i];
        uint32_t q;
        dst[0] = (uint64_t)(v->rgba & 0xFF) | (uint64_t)((v->rgba >> 8) & 0xFF) << 32;
        dst[1] = (uint64_t)((v->rgba >> 16) & 0xFF) | (uint64_t)((v->rgba >> 24) & 0xFF) << 32;
        dst += 2;
        if (r->tme && r->fst) {
            dst[0] = (uint64_t)((int)v->s & 0x3FFF) | (uint64_t)((int)v->t & 0x3FFF) << 32;
            dst[1] = 0;
            dst += 2;
        } else if (r->tme) {
            memcpy(&dst[0], &v->s, 4);
            memcpy((uint8_t *)&dst[0] + 4, &v->t, 4);
            memcpy(&q, &v->q, 4);
            dst[1] = q;
            dst += 2;
        }
        dst[0] = (uint64_t)v->x | (uint64_t)v->y << 32;
        dst[1] = v->z & 0xFFFFFF;
        dst += 2;
    }
    if (clamp) { /* (CLAMP 0 after the corners, A+D) */
        dst[0] = 1 | 1ull << 15 | 1ull << 60;
        dst[1] = 0xE;
        dst[2] = 0;
        dst[3] = 0x08 + (r->ctx & 1);
        dst += 4;
    }
    walk(start, (unsigned)(dst - start) / 2, FOLLOW); /* (what it sets, as a packet that stays) */
    gRndOtStat[0]++;
    return dst;
}

void Rnd_Vtx(RndVtx *o, int ctx, int32_t x, int32_t y, uint32_t z, float s, float t, float q, uint32_t rgba) {
    o->x = (float)((x & 0xFFFF) - (int32_t)(sXyOff[ctx & 1] & 0xFFFF)) / 16.0f;
    o->y = (float)((y & 0xFFFF) - (int32_t)((sXyOff[ctx & 1] >> 32) & 0xFFFF)) / 16.0f;
    o->z = z;
    o->s = s;
    o->t = t;
    o->q = q;
    o->rgba = rgba;
}

uint64_t Rnd_Tex0(int ctx) {
    return sTex0[ctx & 1];
}

uint32_t Rnd_Prim(uint32_t prim) {
    return (sPrmodeCont & 1) ? prim & 0x7FF : (prim & 7) | ((uint32_t)sPrmode & 0x7F8);
}

int Rnd_Draw(RndPoly *p, int ctx, int abe, int gouraud) {
    ctx &= 1;
    if (!Port_TakesRecords() || gRndCtxBad[ctx] != 0) {
        return 0;
    }
    {
        uint16_t vu = (uint16_t)(p->st.flags & RND_VU);
        p->st = gRndCtx[ctx];
        p->st.flags = (uint16_t)((p->st.flags & ~(RND_GOURAUD | RND_VU)) | (gouraud ? RND_GOURAUD : 0) | vu);
    }
    if (!abe) {
        p->st.blend = RND_BLEND_NONE;
    }
    p->gsCtx = (uint32_t)ctx;
    return Port_Poly(p);
}

int Rnd_Rect(int ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t z, uint32_t rgba, int abe) {
    RndPoly r;
    int i;

    memset(&r, 0, sizeof(r));
    for (i = 0; i < 4; i++) {
        Rnd_Vtx(&r.v[i], ctx, (i & 1) ? x1 : x0, (i & 2) ? y1 : y0, z, 0.0f, 0.0f, 1.0f, rgba);
    }
    r.n = 4;
    return Rnd_Draw(&r, ctx, abe, 0);
}

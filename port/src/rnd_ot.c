/*
 * The ordering table's packets as polygons (include/port/rnd.h). The effects (src/battle/eft_*.c) queue GS packets
 * in the table (src/sys/gfx_ot.c) that are each a GIF tag and the vertices of a few triangles under the state of
 * the sorted pass. While the table is sent (Ot_Send) each one is read here: a draw whose registers are all known is
 * handed to the renderer as polygons (Port_Poly), in its place among the packets that stay; any other packet stays,
 * and its register writes are followed, as the state of the draws after it (the chains' heads set the blend of
 * their layer; the effects set CLAMP, TEX1, TEX0). Nothing is read from the GS: the state is the table's own.
 *
 * A draw taken leaves the GS as the packet would not: its TEX0 (a palette load among it), CLAMP, TEX1 and PRIM are
 * written as A+D before the next packet that stays, so what is still a packet draws as it did.
 */
#include <stdint.h>
#include <string.h>
#include "../../include/port/rnd.h"

unsigned gRndOtStat[2];

/* The GS registers followed, per context: */
enum { R_ALPHA, R_TEST, R_FRAME, R_ZBUF, R_SCISSOR, R_FBA, R_TEX1, R_CLAMP, R_TEX0, R_N };
static uint64_t sReg[2][R_N];
static uint64_t sPrim;                     /* PRIM */
static uint32_t sDirty;                    /* registers a draw taken set that the GS does not have yet: bit ctx * R_N + r, 31 PRIM */
static int sUnknown;                       /* a register written that is not followed: the rest stays packets */

static const uint8_t sAddr[R_N] = {0x42, 0x47, 0x4C, 0x4E, 0x40, 0x4A, 0x14, 0x08, 0x06}; /* context 1's; + 1 for 2 */

void Rnd_OtBegin(uint64_t frame, uint64_t scissor) {
    int c;
    for (c = 0; c < 2; c++) { /* as Ot_AddEnv(0) */
        sReg[c][R_ALPHA] = 0x44;
        sReg[c][R_TEST] = 0x5000C;
        sReg[c][R_FRAME] = frame;
        sReg[c][R_ZBUF] = 0xE0 | 0x31ull << 24 | 1ull << 32;
        sReg[c][R_SCISSOR] = scissor;
        sReg[c][R_FBA] = 0;
        sReg[c][R_TEX1] = 0x60;
        sReg[c][R_CLAMP] = 0;
        sReg[c][R_TEX0] = 0;
    }
    sPrim = 0;
    sDirty = 0;
    sUnknown = !Port_TakesRecords();
}

/* Register addr written with v: followed (1), or not (0). (Some leave nothing a draw here reads.) */
static int set_reg(uint32_t addr, uint64_t v, int taken) {
    int r, c;
    if (addr == 0x00) {
        sPrim = v;
        sDirty = taken ? sDirty | 1u << 31 : sDirty & ~(1u << 31);
        return 1;
    }
    for (r = 0; r < R_N; r++) {
        for (c = 0; c < 2; c++) {
            if (addr == sAddr[r] + (uint32_t)c) {
                sReg[c][r] = v;
                sDirty = taken ? sDirty | 1u << (c * R_N + r) : sDirty & ~(1u << (c * R_N + r));
                return 1;
            }
        }
    }
    return addr == 0x3F || addr == 0x46 || addr == 0x45 || addr == 0x0F; /* TEXFLUSH, COLCLAMP, DTHE, NOP */
}

/* RndState from context c's registers; 0 = one it cannot say. */
static int state_of(RndState *st, int c, int abe) {
    const uint64_t *g = sReg[c];
    uint64_t a = g[R_ALPHA] & 0xFF, t = g[R_TEST], m = g[R_FRAME] >> 32, k = g[R_CLAMP] & 15;
    static const uint8_t atst[8] = {RND_ATEST_NEVER, RND_ATEST_ALWAYS, 2, 3, 4, 5, 6, 7}; /* the GS's ATST -> RND_ATEST_ */

    memset(st, 0, sizeof(*st));
    st->fbp = (uint32_t)g[R_FRAME] & 0x1FF;
    if (m != 0 && m != 0x00FFFFFF && m != 0xFF000000u) {
        return 0;
    }
    st->flags = (m == 0x00FFFFFF ? RND_NO_COLOUR : 0) | (m == 0xFF000000u ? RND_NO_ALPHA : 0) |
                ((g[R_ZBUF] >> 32) & 1 ? 0 : RND_ZWRITE) | (g[R_FBA] & 1 ? RND_MARK : 0) |
                ((g[R_TEX1] >> 5) & 1 ? RND_SMOOTH : 0) | (k == 0 ? RND_REPEAT : 0) | ((sPrim >> 3) & 1 ? RND_GOURAUD : 0);
    if (k != 0 && k != 5) { /* (a region clamp) */
        return 0;
    }
    if (!abe) {
        st->blend = RND_BLEND_NONE;
    } else {
        uint64_t b = a & ~0x30ull;
        st->blend = b == 0x44 ? RND_BLEND_NORMAL : b == 0x48 ? RND_BLEND_ADD : b == 0x42 ? RND_BLEND_SUB : b == 0x49 ? RND_BLEND_DEST : 0xFF;
        if (st->blend == 0xFF || ((a >> 4) & 3) > 1) {
            return 0;
        }
        st->flags |= (a >> 4) & 1 ? RND_DEST_ALPHA : 0;
    }
    if (t & 1) {
        st->atest = atst[(t >> 1) & 7];
        st->aref = (uint8_t)(t >> 4);
        st->afail = (uint8_t)((t >> 12) & 3);
    }
    st->mask = (t >> 14) & 1 ? ((t >> 15) & 1 ? RND_IF_MARKED : RND_IF_UNMARKED) : RND_MASK_NONE;
    if ((t >> 16) & 1) {
        uint32_t z = (uint32_t)(t >> 17) & 3;
        if (z == 0) { /* (never) */
            return 0;
        }
        st->ztest = z == 1 ? RND_Z_ALWAYS : z == 2 ? RND_Z_GEQUAL : RND_Z_GREATER;
    }
    st->scissor[0] = (int16_t)(g[R_SCISSOR] & 0x7FF);
    st->scissor[2] = (int16_t)((g[R_SCISSOR] >> 16) & 0x7FF);
    st->scissor[1] = (int16_t)((g[R_SCISSOR] >> 32) & 0x7FF);
    st->scissor[3] = (int16_t)((g[R_SCISSOR] >> 48) & 0x7FF);
    return 1;
}

/* A corner as the GS takes it: x, y in sixteenths of a GS pixel (the window at XYOFFSET 0x7000, 0x7200). */
static void gs_vtx(RndVtx *o, uint32_t x, uint32_t y, uint32_t z, float s, float t, float q, uint32_t rgba) {
    o->x = (float)((int32_t)(x & 0xFFFF) - 0x7000) / 16.0f;
    o->y = (float)((int32_t)(y & 0xFFFF) - 0x7200) / 16.0f;
    o->z = z;
    o->s = s;
    o->t = t;
    o->q = q;
    o->rgba = rgba;
}

/* How a packet is read: whether it can be taken, its polygons to the queue, or only its register writes
   (a packet that stays). */
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
    uint64_t t0 = sReg[c][R_TEX0];
    RndVtx v;
    RndPoly p;

    if (mode == FOLLOW) {
        return 1;
    }
    if (type < 3 || type > 6 || ((sPrim >> 5) & 1)) { /* (points, lines; fog) */
        return 0;
    }
    if (tme && fst) {
        float tw = (float)(1u << ((t0 >> 26) & 15)), th = (float)(1u << ((t0 >> 30) & 15));
        gs_vtx(&v, x, y, z, (float)sU / 16.0f / tw, (float)sVv / 16.0f / th, 1.0f, sRgba);
    } else {
        gs_vtx(&v, x, y, z, sS, sT, sQ, sRgba);
    }
    if (sVn == (type == 6 ? 2 : 3)) { /* the queue full: a fan keeps its first, the others their last two */
        if (type == 5) {
            sV[1] = sV[2];
        } else {
            sV[0] = sV[1];
            sV[1] = sV[2];
        }
        sVn--;
    }
    sV[sVn++] = v;
    if (!kick || sVn < (type == 6 ? 2 : 3)) {
        return 1;
    }
    memset(&p, 0, sizeof(p));
    if (!state_of(&p.st, c, (int)(sPrim >> 6) & 1)) {
        return 0;
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
    } else {
        memcpy(p.v, sV, sizeof(sV));
        p.n = 3;
    }
    if (type == 3 || type == 6) {
        sVn = 0;
    }
    p.gsTex0 = tme ? t0 : 0;
    p.gsCtx = (uint32_t)c;
    if (mode == DRAW) {
        Port_Poly(&p);
    }
    return 1;
}

/* Reads one packet's GIF data. Returns the vertices that draw, or -1: one it cannot read (or draw). */
static int walk(const uint64_t *d, unsigned qwc, int mode) {
    const uint64_t *end = d + qwc * 2;
    int kicks = 0, ok = 1;

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
            set_reg(0, (tag >> 47) & 0x7FF, mode == DRAW);
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
                    case 0: case 6: case 7: case 8: case 9: set_reg(r, lo, mode == DRAW); break;
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
                    case 0: set_reg(0, lo & 0x7FF, mode == DRAW); break;
                    case 6: case 7: case 8: case 9: set_reg(r, lo, mode == DRAW); break;
                    case 14: /* A+D */
                        if ((hi & 0xFF) <= 0x05 || (hi & 0xFF) == 0x0C || (hi & 0xFF) == 0x0D) { /* (vertex registers by address) */
                            ok = 0;
                        } else if (!set_reg((uint32_t)hi & 0xFF, lo, mode == DRAW)) {
                            ok = 0;
                            if (mode == FOLLOW) { sUnknown = 1; } /* (what it set is not followed: the rest stays packets) */
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
        if ((tag >> 15) & 1) { /* EOP */
            break;
        }
    }
    return ok ? kicks : -1;
}

int Rnd_OtTake(const uint64_t *pkt, unsigned qwc) {
    uint64_t save[2][R_N], prim = sPrim;
    uint32_t dirty = sDirty;
    int n;

    if (sUnknown) {
        return 0;
    }
    memcpy(save, sReg, sizeof(save));
    n = walk(pkt, qwc, CHECK);
    memcpy(sReg, save, sizeof(save));
    sPrim = prim;
    sDirty = dirty;
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
    for (b = 0; b < 2 * R_N; b++) {
        if (sDirty >> b & 1) {
            dst[0] = sReg[b / R_N][b % R_N];
            dst[1] = sAddr[b % R_N] + (uint64_t)(b / R_N);
            dst += 2;
            n++;
        }
    }
    if (sDirty >> 31 & 1) {
        dst[0] = sPrim;
        dst[1] = 0;
        dst += 2;
        n++;
    }
    tag[0] = n | 1ull << 15 | 1ull << 60; /* NLOOP n, EOP, PACKED, one register: A+D */
    tag[1] = 0xE;
    sDirty = 0;
    return dst;
}

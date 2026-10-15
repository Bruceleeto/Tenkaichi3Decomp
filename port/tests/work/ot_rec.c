/*
 * The ordering table's records against its packets: each draw is sent once as the GIF packet an effect writes
 * (read back by Rnd_OtTake / Rnd_OtDraw) and once as the record that stands for it (Rnd_OtRec), and the polygons
 * handed to Port_Poly, and the registers left to write before the next packet (Rnd_OtState), are compared. The
 * record's own packet (Rnd_OtRecPacket, for a state that cannot be drawn) is read back the same way and compared too.
 *
 * And the packet memory the nodes come from (src/sys/gfx_ot.c Ot_RecRoom, Ot_NewRec, Ot_NewAd): when it is full a
 * node is not taken (NULL), the cursor stays, nothing is linked, and the turned-away draws are counted.
 *
 *   gcc -m32 -O1 -Iinclude -Iport/include port/tests/work/ot_rec.c -lm -o /tmp/ot_rec && /tmp/ot_rec
 */
#include "../../src/rnd_gs.c"
#include "port_compat.h"
#include "../../../src/sys/gfx_ot.c"

/* What gfx_ot.c needs from the rest of the game, for its record nodes alone. */
Ot *gOt;
OtPrim *gOtHead, *gOtTail;
u32 *gOtCur;
OtSlot *gOtFirst, *gOtZ, *gOtLast;
Gfx gGfx;
OtView *gBtlCamView;
s32 BtlStage_IsReady(void) { return 0; }
void Dma_AddData(void *src, s32 size) { (void)src; (void)size; }
u64 *Dma_BeginDirect(void) { return NULL; }
void Dma_EndDirect(u64 *end) { (void)end; }
void *Heap_Alloc(s32 size, u32 align, s32 fromTail, s32 heap) { (void)size; (void)align; (void)fromTail; (void)heap; return NULL; }
void Heap_Free(void *ptr) { (void)ptr; }
s32 Ot_Reset(void) { return 1; }
static int sOtBytes;
int Port_OtBytes(void) { return sOtBytes; }

RndState gRndCtx[2];
static int sTakes = 1;
int Port_TakesRecords(void) { return sTakes; }

static RndPoly sGot[2][16];
static int sGotN[2], sSide;
int Port_Poly(const RndPoly *p) {
    if (sGotN[sSide] < 16) {
        sGot[sSide][sGotN[sSide]++] = *p;
    }
    return 1;
}

static uint32_t sSeed = 12345;
static uint32_t rnd(void) {
    sSeed = sSeed * 1103515245u + 12345u;
    return sSeed >> 8;
}

/* The same starting state before each side. */
static void reset(void) {
    int c;
    memset(gRndCtx, 0, sizeof(gRndCtx));
    for (c = 0; c < 2; c++) {
        gRndCtx[c].blend = RND_BLEND_ADD;
        gRndCtx[c].flags = RND_SMOOTH;
        gRndCtx[c].scissor[2] = 511;
        gRndCtx[c].scissor[3] = 447;
        sTex0[c] = 0x123456789Aull + c;
        sXyOff[c] = (uint64_t)(0x7200 + c * 16) << 32 | (0x7000 + c * 32);
    }
    memset(gRndCtxBad, 0, sizeof(gRndCtxBad));
    sPrim = 0;
    sDirty = 0;
    sDirtyPrim = 0;
    memset(sDirtyVal, 0, sizeof(sDirtyVal));
    sVn = 0;
    sRgba = 0;
    sS = sT = 0.0f;
    sQ = 1.0f;
    sU = sVv = 0;
}

/* The packet an effect writes: REGLIST (PRIM, TEX0, then each corner's RGBAQ, ST or UV, XYZF2), or PACKED. */
static unsigned packet(uint64_t *d, const RndOtRec *r, int packed) {
    uint64_t *s = d, prim = (uint64_t)r->type | (uint64_t)r->gouraud << 3 | (uint64_t)r->tme << 4 | (uint64_t)r->abe << 6 |
                            (uint64_t)r->fst << 8 | (uint64_t)r->ctx << 9;
    int tex = r->setTex0 & 1, clamp = (r->setTex0 & RND_OTREC_CLAMP) != 0;
    int i, nreg = 1 + tex + 2 * clamp + r->n * (r->tme ? 3 : 2);
    uint64_t regs = 0;
    int k = 0;

    regs |= 0ull << (k++ * 4);
    if (tex) {
        regs |= (uint64_t)(6 + r->ctx) << (k++ * 4);
    }
    if (clamp) { /* (EftSprAnim_QueueTri's packet: CLAMP 5 before the corners, CLAMP 0 after) */
        regs |= (uint64_t)(8 + r->ctx) << (k++ * 4);
    }
    if (!packed) {
        for (i = 0; i < r->n; i++) {
            regs |= 1ull << (k++ * 4);
            if (r->tme) {
                regs |= (uint64_t)(r->fst ? 3 : 2) << (k++ * 4);
            }
            regs |= 4ull << (k++ * 4);
        }
        if (clamp) {
            regs |= (uint64_t)(8 + r->ctx) << (k++ * 4);
        }
        d[0] = 1 | 1ull << 15 | 1ull << 58 | (uint64_t)(nreg & 15) << 60;
        d[1] = regs;
        d += 2;
        *d++ = prim;
        if (tex) {
            *d++ = r->tex0;
        }
        if (clamp) {
            *d++ = 5;
        }
        for (i = 0; i < r->n; i++) {
            const RndOtVtx *v = &r->v[i];
            *d = v->rgba;
            memcpy((uint8_t *)d + 4, &v->q, 4);
            d++;
            if (!r->tme) {
            } else if (r->fst) {
                *d++ = (uint64_t)((int)v->s & 0x3FFF) | (uint64_t)((int)v->t & 0x3FFF) << 16;
            } else {
                memcpy(d, &v->s, 4);
                memcpy((uint8_t *)d + 4, &v->t, 4);
                d++;
            }
            *d++ = (uint64_t)v->x | (uint64_t)v->y << 16 | (uint64_t)v->z << 32 | 0xFFull << 56;
        }
        if (clamp) {
            *d++ = 0;
        }
        if ((d - s) & 1) {
            *d++ = 0;
        }
        return (unsigned)(d - s) / 2;
    }
    /* PACKED: PRIM and TEX0 first, then RGBAQ, ST (Q in it), XYZF2 per corner, ADC 0 */
    d[0] = 1 | (uint64_t)(1 + (r->setTex0 != 0)) << 60;
    d[1] = r->setTex0 ? (uint64_t)(6 + r->ctx) << 4 : 0;
    d[2] = prim;
    d[3] = 0;
    d += 4;
    if (r->setTex0) {
        d[0] = r->tex0;
        d[1] = 0;
        d += 2;
    }
    d[0] = (uint64_t)r->n | 1ull << 15 | (r->tme ? 3ull : 2ull) << 60;
    d[1] = r->tme ? 0x1 | 0x2 << 4 | 0x4 << 8 : 0x1 | 0x4 << 4;
    d += 2;
    for (i = 0; i < r->n; i++) {
        const RndOtVtx *v = &r->v[i];
        uint32_t q;
        d[0] = (v->rgba & 0xFF) | (uint64_t)((v->rgba >> 8) & 0xFF) << 32;
        d[1] = ((v->rgba >> 16) & 0xFF) | (uint64_t)(v->rgba >> 24) << 32;
        d += 2;
        if (r->tme) {
            memcpy(&d[0], &v->s, 4);
            memcpy((uint8_t *)&d[0] + 4, &v->t, 4);
            memcpy(&q, &v->q, 4);
            d[1] = q;
            d += 2;
        }
        d[0] = v->x | (uint64_t)v->y << 32;
        d[1] = (uint64_t)v->z << 4 | 0xFFull << 36;
        d += 2;
    }
    return (unsigned)(d - s) / 2;
}

/* What is left to write after a side: the registers (Rnd_OtState), and the state followed. */
typedef struct After {
    uint64_t st[32];
    uint64_t tex0[2], prim;
    RndState ctx[2];
} After;

static void after(After *a) {
    memset(a, 0, sizeof(*a));
    Rnd_OtState(a->st);
    memcpy(a->tex0, sTex0, sizeof(a->tex0));
    a->prim = sPrim;
    memcpy(a->ctx, gRndCtx, sizeof(a->ctx));
}

static int sFail, sCases, sPolys;

static void compare(const char *what, unsigned prim, After *a, After *b) {
    int i;
    if (sGotN[0] != sGotN[1]) {
        printf("FAIL %s prim %03X: %d polygons from the packet, %d from the record\n", what, prim, sGotN[0], sGotN[1]);
        sFail++;
        return;
    }
    sPolys += sGotN[0];
    for (i = 0; i < sGotN[0]; i++) {
        int j, side;
        for (side = 0; side < 2; side++) {
            for (j = 0; j < 4 && sGot[side][i].gsTex0 == 0; j++) { /* (untextured: s t q are what the GS last had, not read) */
                sGot[side][i].v[j].s = sGot[side][i].v[j].t = sGot[side][i].v[j].q = 0.0f;
            }
        }
        if (memcmp(&sGot[0][i], &sGot[1][i], sizeof(RndPoly)) != 0) {
            printf("FAIL %s prim %03X: polygon %d differs\n", what, prim, i);
            sFail++;
            return;
        }
    }
    if (memcmp(a, b, sizeof(*a)) != 0) {
        printf("FAIL %s prim %03X: the state after differs\n", what, prim);
        sFail++;
    }
}

static void one(unsigned prim, int n, int setTex0, int packed, int bad) {
    RndOtRec r;
    uint64_t pkt[256];
    unsigned qwc;
    After a, b;
    int i, took;

    memset(&r, 0, sizeof(r));
    r.type = prim & 7;
    r.gouraud = (prim >> 3) & 1;
    r.tme = (prim >> 4) & 1;
    r.abe = (prim >> 6) & 1;
    r.fst = (prim >> 8) & 1;
    r.ctx = (prim >> 9) & 1;
    r.n = (uint8_t)n;
    r.setTex0 = (uint8_t)setTex0;
    r.tex0 = (uint64_t)rnd() << 32 | rnd() | 0x44000000ull;
    for (i = 0; i < n; i++) {
        r.v[i].x = (uint16_t)(0x6000 + rnd() % 0x2000);
        r.v[i].y = (uint16_t)(0x6000 + rnd() % 0x2000);
        r.v[i].z = rnd() & 0xFFFFFF;
        if (r.fst) {
            r.v[i].s = (float)(rnd() & 0x3FFF);
            r.v[i].t = (float)(rnd() & 0x3FFF);
            r.v[i].q = 1.0f;
        } else {
            r.v[i].s = (float)(rnd() % 1000) / 999.0f;
            r.v[i].t = (float)(rnd() % 1000) / 999.0f;
            r.v[i].q = 0.5f + (float)(rnd() % 100) / 100.0f;
        }
        r.v[i].rgba = rnd() ^ rnd() << 8;
    }
    qwc = packet(pkt, &r, packed);
    sCases++;

    /* the packet, read back */
    reset();
    gRndCtxBad[r.ctx] = bad;
    sSide = 0;
    sGotN[0] = 0;
    took = Rnd_OtTake(pkt, qwc);
    if (took) {
        Rnd_OtDraw(pkt, qwc);
    }
    after(&a);

    /* the record */
    reset();
    gRndCtxBad[r.ctx] = bad;
    sSide = 1;
    sGotN[1] = 0;
    if (Rnd_OtRecTakes(&r) != took) {
        printf("FAIL prim %03X: the packet %s taken, the record %s\n", prim, took ? "is" : "is not", took ? "is not" : "is");
        sFail++;
        return;
    }
    if (took) {
        Rnd_OtRec(&r);
        after(&b);
        compare(packed ? "packed" : "reglist", prim, &a, &b);
    } else {
        uint64_t mine[256], *end = Rnd_OtRecPacket(&r, mine);
        After c;
        after(&b);
        compare("not taken", prim, &a, &b);
        /* its own packet, taken where the state allows it, draws as the producer's */
        reset();
        sSide = 0;
        sGotN[0] = 0;
        if (Rnd_OtTake(pkt, qwc)) {
            Rnd_OtDraw(pkt, qwc);
        }
        after(&a);
        reset();
        sSide = 1;
        sGotN[1] = 0;
        if (Rnd_OtTake(mine, (unsigned)(end - mine) / 2)) {
            Rnd_OtDraw(mine, (unsigned)(end - mine) / 2);
        }
        after(&c);
        compare("record's packet", prim, &a, &c);
        if (sGotN[0] == 0 && sTakes) {
            printf("FAIL prim %03X: nothing drawn from either packet\n", prim);
            sFail++;
        }
    }
}

/* A packet memory of 0x400 bytes made to seem 0x200 long (BT3_OT_BYTES): records of four corners and A+D nodes taken
   until the next does not fit, linked as a producer links them; then the next of each is NULL, the cursor stays,
   the chain is as it was, and the two are counted until Ot_ResetCursor rewinds. */
static void full(void) {
    static u32 mem[0x400 / 4] __attribute__((aligned(64)));
    static Ot ot;
    OtPrim head, *p, *last;
    OtEntry e;
    u32 *before;
    int taken = 0;

    memset(&ot, 0, sizeof(ot));
    ot.buf = mem;
    ot.bufSize = sizeof(mem);
    gOt = &ot;
    gOtCur = NULL;
    sCases++;
    if (Ot_NewRec(0x5C, 4) != NULL || Ot_NewAd(8, 5) != NULL || Ot_RecRoom(16)) {
        printf("FAIL full: a node taken before there is packet memory\n");
        sFail++;
    }
    sOtBytes = 0x200;
    gOtCur = mem;
    head.next = NULL;
    e.head = e.tail = &head;
    while (Ot_RecRoom(Ot_RecBytes(4))) {
        p = Ot_NewRec(0x5C, 4);
        if (p == NULL) {
            printf("FAIL full: Ot_RecRoom has room, Ot_NewRec none\n");
            sFail++;
            return;
        }
        e.tail->next = p;
        e.tail = p;
        taken++;
    }
    while (Ot_RecRoom(Ot_AdBytes())) {
        p = Ot_NewAd(8, 5);
        e.tail->next = p;
        e.tail = p;
        taken++;
    }
    last = e.tail;
    before = gOtCur;
    if ((u8 *)gOtCur > (u8 *)mem + 0x200 || (u8 *)gOtCur + Ot_AdBytes() <= (u8 *)mem + 0x200) {
        printf("FAIL full: the cursor at %d bytes after %d nodes\n", (int)((u8 *)gOtCur - (u8 *)mem), taken);
        sFail++;
    }
    sOtRecFull = 0;
    if (Ot_NewRec(0x5C, 4) != NULL || Ot_NewAd(8, 5) != NULL || Ot_NewRec(0x46, 2) != NULL) {
        printf("FAIL full: a node taken from a full packet memory\n");
        sFail++;
    }
    if (gOtCur != before || e.tail != last || last->next != NULL) {
        printf("FAIL full: the cursor or the chain moved\n");
        sFail++;
    }
    if (sOtRecFull != 3) {
        printf("FAIL full: %d turned away counted, not 3\n", sOtRecFull);
        sFail++;
    }
    for (p = head.next; p != NULL && p != last; p = p->next) {
        taken--;
    }
    if (p != last || taken != 1) {
        printf("FAIL full: the chain does not hold the nodes taken\n");
        sFail++;
    }
    Ot_ResetCursor();
    if (sOtRecFull != 0 || gOtCur != mem || Ot_NewRec(0x5C, 4) == NULL) {
        printf("FAIL full: Ot_ResetCursor did not rewind and clear the count\n");
        sFail++;
    }
    gOt = NULL;
    gOtCur = NULL;
}

int main(void) {
    static const struct { unsigned prim; int n; } kinds[] = {
        {0x00B, 3}, {0x04B, 3}, {0x24B, 3}, {0x01B, 3}, {0x05B, 3}, {0x25B, 3},
        {0x014, 4}, {0x01C, 4}, {0x054, 4}, {0x05C, 4}, {0x25C, 4}, {0x21C, 4}, {0x254, 4},
        {0x046, 2}, {0x246, 2}, {0x056, 2}, {0x156, 2},
        {0x001, 2}, {0x041, 2}, {0x002, 3}, {0x042, 4},
        {0x15C, 4}, {0x005, 4}, {0x01D, 4},
    };
    unsigned k;
    int rep, setTex0, packed, bad;

    for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        for (rep = 0; rep < 20; rep++) {
            for (setTex0 = 0; setTex0 < 4; setTex0++) { /* (bit 0 TEX0, bit 1 RND_OTREC_CLAMP) */
                for (packed = 0; packed < 2; packed++) {
                    if (packed && (setTex0 & RND_OTREC_CLAMP)) {
                        continue; /* (only a REGLIST packet writes CLAMP around its corners) */
                    }
                    if (packed && ((kinds[k].prim >> 8) & 1)) {
                        continue; /* (the PACKED writer here has no UV) */
                    }
                    for (bad = 0; bad < 2; bad++) {
                        one(kinds[k].prim, kinds[k].n, setTex0, packed, bad ? RND_BAD_BLEND : 0);
                    }
                }
            }
        }
    }
    sTakes = 0;
    one(0x05C, 4, 1, 0, 0);
    full();
    printf("%d cases (%d polygons compared), %d failed; records drawn %u, sent as packets %u\n", sCases, sPolys, sFail, gRndOtStat[2], gRndOtStat[0]);
    return sFail != 0;
}

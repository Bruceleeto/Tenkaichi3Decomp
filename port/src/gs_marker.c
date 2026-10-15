/*
 * Markers for the PC renderer inside the game's display list. The game builds each frame's list first and the
 * renderer executes it afterwards, so "a native effect goes HERE" has to travel in the list: a one-register GIF
 * packet that writes the effect number to GS register 0x7F, which the real GS does not have. The front end
 * (port/src/gs/gs_core.c) hands it to the GPU back end; the software reference ignores it and keeps drawing the
 * PS2 passes. Called from the game's effect functions under `#ifdef PORT`.
 */
#include <stdint.h>

extern void Dma_AddData(void *src, int size);

void Port_GsMarker(int effect) {
    uint32_t pkt[12] = {
        0x10000000u | 2, 0, 0x10000000u, 0x50000000u | 2, /* DMA CNT, 2 quadwords; VIF FLUSHE, DIRECT 2 */
        0x8000u | 1, 0x10000000u, 0xE, 0,                  /* GIF tag: 1 loop, EOP, one register: A+D */
        (uint32_t)effect, 0, 0x7F, 0,                      /* value, register number */
    };

    Dma_AddData(pkt, sizeof(pkt));
}

/* include/port/rnd.h: the quad to the renderer's queue (gs/gs_core.c Gs_RecordPush), with the place in the display
   list it belongs at: the list being built (its first address) and the list's cursor (where the next packet goes). */
#include <string.h>
#include "../../include/port/rnd.h"
#include "port_addr.h"

extern int Gs_TakesRecords(void);                                       /* gs/gs_core.c */
extern int Gs_RecordPush(const RndQuad *q, uint32_t list, uint32_t at); /* gs/gs_core.c */
extern uint32_t *gDmaCur;     /* src/sys/dma.c: the display list's cursor */
extern uint32_t gDmaBuf[2];   /* its two buffers (game addresses) */
extern int gDmaBufIdx;        /* the one being built */

RndState gRndCtx[2];
int gRndCtxValid;

int Port_Quad(const RndQuad *q) {
    if (!Gs_TakesRecords()) {
        return 0;
    }
    return Gs_RecordPush(q, gDmaBuf[gDmaBufIdx], Port_Addr(gDmaCur));
}

int Port_TakesRecords(void) {
    return Gs_TakesRecords();
}

void Rnd_StateInit(RndState *s, uint32_t fbp) {
    memset(s, 0, sizeof(*s));
    s->fbp = fbp;
    s->scissor[2] = 511;
    s->scissor[3] = 447;
}

int Rnd_Fill(const RndState *st, uint32_t rgba) {
    RndQuad r;
    int i;

    memset(&r, 0, sizeof(r));
    r.st = *st;
    for (i = 0; i < 4; i++) {
        r.xy[i][0] = (i & 1) ? 512.0f : 0.0f;
        r.xy[i][1] = (i & 2) ? 448.0f : 0.0f;
        r.rgba[i] = rgba;
    }
    return Port_Quad(&r);
}

int gRndSprites;

int Rnd_Sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, uint32_t rgba, uint32_t tex,
               uint32_t pal, uint64_t tex0) {
    RndQuad r;
    int i;

    if (!gRndSprites) {
        return 0;
    }
    memset(&r, 0, sizeof(r));
    r.st = gRndCtx[0];
    for (i = 0; i < 4; i++) {
        r.xy[i][0] = (float)(((i & 1) ? x1 : x0) - 0x700);
        r.xy[i][1] = (float)(((i & 2) ? y1 : y0) - 0x720);
        r.uv[i][0] = (int16_t)((i & 1) ? u1 : u0);
        r.uv[i][1] = (int16_t)((i & 2) ? v1 : v0);
        r.rgba[i] = rgba;
    }
    r.tex = tex;
    r.pal = pal;
    r.gsTex0 = tex0;
    Port_Quad(&r);
    return 1;
}

void Rnd_DefaultEnv(uint32_t fbp) {
    RndState *s = &gRndCtx[0];
    Rnd_StateInit(s, fbp);
    s->flags = RND_ZWRITE | RND_SMOOTH; /* (ZBUF without mask; TEX1 0x60; CLAMP clamped) */
    s->ztest = RND_Z_GEQUAL;            /* (TEST 0x50000) */
}

void Rnd_SetTest(int ctx, int ztest, int atest, int aref, int afail, int mask) {
    RndState *s = &gRndCtx[ctx & 1];
    s->ztest = (uint8_t)ztest;
    s->atest = (uint8_t)atest;
    s->aref = (uint8_t)aref;
    s->afail = (uint8_t)afail;
    s->mask = (uint8_t)mask;
}

void Rnd_SetWrite(int ctx, uint32_t fbp, int noWrite) {
    RndState *s = &gRndCtx[ctx & 1];
    s->fbp = fbp;
    s->flags = (uint16_t)((s->flags & ~(RND_NO_COLOUR | RND_NO_ALPHA)) | (noWrite & (RND_NO_COLOUR | RND_NO_ALPHA)));
}

void Rnd_SetMark(int ctx, int on) {
    Rnd_SetFlag(ctx, RND_MARK, on);
}

void Rnd_SetBlend(int ctx, int blend, int destAlpha) {
    gRndCtx[ctx & 1].blend = (uint8_t)blend;
    Rnd_SetFlag(ctx, RND_DEST_ALPHA, destAlpha);
}

void Rnd_SetFlag(int ctx, int flag, int on) {
    RndState *s = &gRndCtx[ctx & 1];
    s->flags = (uint16_t)(on ? s->flags | flag : s->flags & ~flag);
}

void Rnd_SetScissor(int ctx, int x0, int x1, int y0, int y1) {
    RndState *s = &gRndCtx[ctx & 1];
    s->scissor[0] = (int16_t)x0;
    s->scissor[1] = (int16_t)y0;
    s->scissor[2] = (int16_t)x1;
    s->scissor[3] = (int16_t)y1;
}

/*
 * Markers for the PC renderer inside the game's display list. The game builds each frame's list first and the
 * renderer executes it afterwards, so "a native effect goes HERE" has to travel in the list: a one-register GIF
 * packet that writes the effect number to GS register 0x7F, which the real GS does not have. The front end
 * (port/src/gs/gs_core.c) hands it to the GPU back end; the software reference ignores it and keeps drawing the
 * PS2 passes. Called from the game's effect functions under `#ifdef PORT`.
 */
#include <stdint.h>

extern void Dma_AddData(void *src, int size);
void Port_MarkReg(uint32_t reg, uint64_t v);

void Port_GsMarker(int effect) {
    Port_MarkReg(0x7F, (uint32_t)effect);
}

/* include/port/rnd.h: the quad to the renderer's queue (gs/gs_walk.c Gs_RecordPush), with the place in the display
   list it belongs at: the list's cursor (where the next packet goes). */
#include <string.h>
#include "../../include/port/rnd.h"
#include "port_addr.h"

extern int Gs_TakesRecords(void);                                       /* gs/gs_walk.c */
extern int Gs_RecordPush(const void *rec, int kind, uint32_t at); /* gs/gs_walk.c */
extern uint32_t *gDmaCur;     /* src/sys/dma.c: the display list's cursor */

RndState gRndCtx[2];
int gRndCtxValid;

/* What the renderer was handed on the records path and did not take, where the PS2 packet the game then builds is
   not read (Port_ListRead 0: the Dreamcast, BT3_WALK=3 on PC): it is not drawn. Never silent: an error on stderr
   once per place it came from (`from`: the code that asked), and counted (gPortLost). `what` says what was lost,
   `why` the decliner's reason (its bits). Where the list is read the packet is drawn instead: nothing to say. */
#include <stdio.h>
#if defined(__linux__)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } PortDlInfo;
extern int dladdr(const void *addr, PortDlInfo *info);
#endif
unsigned gPortLost;

void Port_Lost(const char *what, uint32_t why, const void *from) {
    static const void *seen[256];
    static unsigned nSeen;
    char name[160];
    unsigned i;

    if (Port_ListRead()) {
        return;
    }
    gPortLost++;
    for (i = 0; i < nSeen; i++) {
        if (seen[i] == from) {
            return;
        }
    }
    if (nSeen < sizeof(seen) / sizeof(seen[0])) {
        seen[nSeen++] = from;
    }
    snprintf(name, sizeof(name), "0x%08x", (unsigned)(uintptr_t)from);
#if defined(__linux__)
    {
        PortDlInfo info;
        if (dladdr(from, &info) && info.dli_sname != NULL) {
            snprintf(name, sizeof(name), "%s+0x%x", info.dli_sname, (unsigned)((uintptr_t)from - (uintptr_t)info.dli_saddr));
        }
    }
#endif
    fprintf(stderr, "bt3: ERROR: not drawn: %s (0x%X), from %s: the renderer did not take it and its PS2 packet is not "
                    "read here\n", what, (unsigned)why, name);
}

/* ---- the textures the game's uploads bound where (gs/gs_pvr.c Port_TexNative, Port_PalNative) ----
   The game's models, and some of its drawing, name a texture by where the PS2 uploaded it: TEX0's TBP and CBP. Each
   upload made native binds its handle to its first block, in the order the game queues them, which is the order
   the list is drawn in; a record that names its texture by TEX0 alone is given the handles bound there when it is
   made (bind_resolve), so the renderer is handed handles and reads no address. An upload over the blocks of an
   earlier one unbinds it, as the GS's memory would lose it. */
#define BIND_MAX 1024
typedef struct Bind {
    uint16_t block, next; /* next: the next of its sBindHash chain + 1 */
    uint32_t h[2];        /* the texture's handle, the palette's; 0 = none */
} Bind;
static Bind sBind[BIND_MAX];
static uint16_t sBindHash[512], sBindFree; /* (records + 1) */
static uint32_t sBindAt[16384 / 32];       /* a bit per block: a binding starts there */
static int sBindInit;
unsigned gRndBindLive, gRndBindPeak, gRndRebound; /* (gs/gs_pvr.c's report) */

static uint16_t *bind_slot(uint32_t b) {
    uint16_t *p = &sBindHash[(b * 2654435761u) >> 23];
    while (*p != 0 && sBind[*p - 1].block != b) {
        p = &sBind[*p - 1].next;
    }
    return p;
}

static void bind_drop(uint32_t b) {
    uint16_t *p = bind_slot(b), i = *p;
    sBindAt[b >> 5] &= ~(1u << (b & 31));
    if (i == 0) {
        return;
    }
    *p = sBind[i - 1].next;
    sBind[i - 1].next = sBindFree;
    sBindFree = i;
    gRndBindLive--;
}

/* gs/gs_pvr.c: handle h (kind 0 a texture, 1 a palette) uploaded to `block`, over `blocks` blocks: those lose what
   was bound at them (0: none do), and `block` holds h. */
void Port_Bind(int kind, uint32_t block, uint32_t blocks, uint32_t h) {
    uint32_t b = block & 0x3FFF, e = b + blocks, w;
    uint16_t *p, i;

    if (!sBindInit) {
        for (i = 0; i < BIND_MAX; i++) {
            sBind[i].next = (uint16_t)(i + 2 <= BIND_MAX ? i + 2 : 0);
        }
        sBindFree = 1;
        sBindInit = 1;
    }
    while (b < e && b < 16384) {
        if ((w = sBindAt[b >> 5] >> (b & 31)) == 0) {
            b = (b | 31) + 1;
            continue;
        }
        b += (uint32_t)__builtin_ctz(w);
        if (b < e) {
            bind_drop(b++);
        }
    }
    b = block & 0x3FFF;
    if (*(p = bind_slot(b)) == 0) {
        if (h == 0) {
            return;
        }
        if ((i = sBindFree) == 0) {
            Port_Lost("a texture's binding: no room for another (BIND_MAX)", b, __builtin_return_address(0));
            return;
        }
        sBindFree = sBind[i - 1].next;
        memset(&sBind[i - 1], 0, sizeof(Bind));
        sBind[i - 1].block = (uint16_t)b;
        *p = i;
        sBindAt[b >> 5] |= 1u << (b & 31);
        if (++gRndBindLive > gRndBindPeak) {
            gRndBindPeak = gRndBindLive;
        }
    }
    sBind[*p - 1].h[kind] = h;
}

static uint32_t bind_at(int kind, uint32_t b) {
    uint16_t i;
    b &= 0x3FFF;
    if (!((sBindAt[b >> 5] >> (b & 31)) & 1) || (i = *bind_slot(b)) == 0) {
        return 0;
    }
    return sBind[i - 1].h[kind];
}

/* The palette bound at cbp as a texture of format psm reads it: a 4-bit one names the first of the four palettes of
   16 in the blocks of a 256 (the font's). */
static uint32_t bind_pal(uint32_t cbp, uint32_t psm) {
    uint32_t h = bind_at(1, cbp), s;
    if ((psm == 0x14 || psm == 0x24 || psm == 0x2C) && (s = Port_PalSub(h, 0)) != 0) {
        h = s;
    }
    return h;
}

/* Palette h is still what is bound at cbp: there, or palette k of 16 of the 256 at cbp - k. */
static int bind_pal_still(uint32_t h, uint32_t cbp) {
    uint32_t k;
    if (bind_at(1, cbp) == h) {
        return 1;
    }
    for (k = 1; k < 4 && k <= cbp; k++) {
        if (Port_PalSub(bind_at(1, cbp - k), (int)k) == h) {
            return 1;
        }
    }
    return 0;
}

/* A record's handles from what TEX0 t0 names: none given, the ones bound at its blocks; a palette given that is no
   longer the one at its CBP (a texture read through a palette: something was uploaded over it), both of them
   anew, as the GS would read what is there now. */
static void bind_resolve(uint32_t *tex, uint32_t *pal, uint64_t t0) {
    uint32_t tbp = (uint32_t)t0 & 0x3FFF, psm = (uint32_t)(t0 >> 20) & 0x3F, cbp = (uint32_t)(t0 >> 37) & 0x3FFF, h;
    int indexed = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;

    if ((*tex & RND_TEX_TARGET) || *tex == RND_TEX_MOVIE) {
        return;
    }
    if (*tex != 0 && (!indexed || (*pal != 0 && bind_pal_still(*pal, cbp)))) {
        return;
    }
    gRndRebound += *tex != 0;
    h = bind_at(0, tbp);
    *tex = h != 0 ? h | (*tex & RND_TEX_RGB) : 0;
    *pal = indexed ? bind_pal(cbp, psm) : 0; /* (also with no texture: a table pass reads the depth page through it,
                                                gs/gs_draw.c depth_clut) */
}

int Port_Quad(const RndQuad *q) {
    RndQuad r;
    if (!Gs_TakesRecords()) {
        return 0;
    }
    if (q->gsTex0 == 0) {
        return Gs_RecordPush(q, 0, Port_Addr(gDmaCur));
    }
    r = *q;
    bind_resolve(&r.tex, &r.pal, r.gsTex0);
    return Gs_RecordPush(&r, 0, Port_Addr(gDmaCur));
}

int Port_Poly(const RndPoly *p) {
    RndPoly r;
    if (!Gs_TakesRecords()) {
        return 0;
    }
    if (p->gsTex0 == 0) {
        return Gs_RecordPush(p, 1, Port_Addr(gDmaCur));
    }
    r = *p;
    bind_resolve(&r.tex, &r.pal, r.gsTex0);
    return Gs_RecordPush(&r, 1, Port_Addr(gDmaCur));
}

extern int Gs_MarksQueued(void); /* gs/gs_walk.c */
int Port_Mark(uint32_t addr, uint64_t d) {
    uint64_t m[2];
    if (!Gs_MarksQueued()) {
        return 0;
    }
    m[0] = addr;
    m[1] = d;
    return Gs_RecordPush(m, 2, Port_Addr(gDmaCur));
}

/* A marker: one of the port's registers (0x7B..0x7F: Port_GsMarker, gs_pvr.c Port_TexNative / Port_PalNative /
   Port_ClutTint) and its value, to the renderer's queue in its place among the records; as a packet (a one-register
   A+D write) into the display list only where the list is read for it (the PS2 reference: gs_core.c; BT3_WALK=1, 2
   on PC). */
extern int Port_ListRead(void); /* gs/gs_walk.c */
void Port_MarkReg(uint32_t reg, uint64_t v) {
    Port_Mark(reg, v);
#ifdef PORT_GS_REF
    if (Port_ListRead()) {
        uint32_t pkt[12] = {
            0x10000000u | 2, 0, 0x10000000u, 0x50000000u | 2, /* DMA CNT, 2 quadwords; VIF FLUSHE, DIRECT 2 */
            0x8000u | 1, 0x10000000u, 0xE, 0,                  /* GIF tag: 1 loop, EOP, one register: A+D */
            (uint32_t)v, (uint32_t)(v >> 32), reg, 0,          /* value, register number */
        };
        Dma_AddData(pkt, sizeof(pkt));
    }
#endif
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

#ifndef PORT_RND_H
#define PORT_RND_H

/*
 * Drawing without GS packets: what the game hands the renderer itself instead of a GIF packet that sends vertices
 * under the GS's registers. The record goes to the renderer's queue (port/src/gs/gs_core.c), not into the display
 * list: it is queued with the place in the list where the packet would have been, and drawn when the walk of the
 * list reaches that place, so it is drawn in the game's order among the draws that are still packets. It becomes a
 * draw of the shared list straight from its state (port/src/gs/gs_draw.c GsGpu_Quad); nothing of the GS's state is
 * read or made for it, but for a texture that has no handles (from GS memory by TEX0, counted).
 *
 * The packets that only SET state are still sent: the draws that are still packets read it. A record replaces a
 * draw, not the state the PS2 would have had after it. BT3_RND_CHECK=1 compares each record's state with the GS's
 * registers where it is drawn and reports what differs (a state the game sets that the record was not told of).
 *
 * Users: the Flash player's quads (src/sys/gfxm_c.c FlashDraw_Quad), the battle HUD's sprites (src/battle/hud_c_c.c),
 * the screen clear and the fades (src/sys/gfx.c Gfx_ClearScreen, src/sys/fade.c Fade_DrawRect), the text and its
 * icons (src/battle/col_c.c, col_c_b.c), the battle menu (src/battle/hud_0_b.c, hud_0_c.c).
 */

#include <stdint.h>

#define RND_BLEND_NORMAL 0 /* Cs * A + Cd * (1 - A); alpha 0x80 = 1 (A: the source's alpha, or RND_DEST_ALPHA's) */
#define RND_BLEND_ADD 1    /* Cs * A + Cd */
#define RND_BLEND_SUB 2    /* Cd - Cs * A */

/* RndState.flags */
#define RND_REPEAT 0x01     /* the texture wraps (else it is clamped at its edges) */
#define RND_SMOOTH 0x02     /* filtered */
#define RND_NO_COLOUR 0x04  /* writes the alpha alone */
#define RND_NO_ALPHA 0x08   /* writes the colour alone */
#define RND_DEST_ALPHA 0x10 /* blended by the alpha already in the picture instead of its own */
#define RND_MARK 0x20       /* marks the pixels it writes (bit 7 of their alpha set: the GS's FBA) */
#define RND_ZWRITE 0x40     /* writes its depth */
#define RND_GOURAUD 0x80    /* colours blended between the corners (else each triangle has its last corner's) */

/* RndState.atest: which texels are drawn, by alpha against aref (0x80 = 1.0). */
#define RND_ATEST_ALWAYS 0
#define RND_ATEST_NEVER 1
#define RND_ATEST_LESS 2
#define RND_ATEST_LEQUAL 3
#define RND_ATEST_EQUAL 4
#define RND_ATEST_GEQUAL 5
#define RND_ATEST_GREATER 6
#define RND_ATEST_NOTEQUAL 7
/* RndState.afail: what a texel that fails it still writes. */
#define RND_AFAIL_NOTHING 0
#define RND_AFAIL_COLOUR 1 /* colour and alpha, not depth */
#define RND_AFAIL_DEPTH 2  /* depth alone */
#define RND_AFAIL_RGB 3    /* the colour without alpha */
/* RndState.ztest, against the depth there (bigger is nearer). */
#define RND_Z_ALWAYS 0
#define RND_Z_GEQUAL 1
#define RND_Z_GREATER 2
/* RndState.mask: drawn only where the pixel's mark (RND_MARK) is ... */
#define RND_MASK_NONE 0
#define RND_IF_UNMARKED 1
#define RND_IF_MARKED 2

typedef struct RndState {
    uint16_t flags;     /* RND_ */
    uint8_t blend;      /* RND_BLEND_ */
    uint8_t atest, aref, afail;
    uint8_t ztest;      /* RND_Z_ */
    uint8_t mask;       /* RND_MASK_NONE, RND_IF_ */
    uint32_t fbp;       /* the frame buffer drawn into (its page) */
    int16_t scissor[4]; /* x0, y0, x1, y1: the pixels drawn, inclusive */
} RndState; /* 20 bytes */

/* A texture is named by the handles of its record (Port_TexHandle, Port_PalHandle). A handle outlives what it
   names: once that is freed it names nothing, and the GS fallback is drawn. */
typedef struct RndQuad {
    float xy[4][2];    /* pixels of the 512 x 448 picture, as a strip: top left, top right, bottom left, bottom right */
    int16_t uv[4][2];  /* sixteenths of a texel, at the same corners */
    uint32_t rgba[4];  /* r in the low byte; 0x80 = 1.0 (the texture's colour times this) */
    uint32_t z;        /* depth, in the 24-bit buffer's steps */
    uint32_t tex;      /* the texture's handle; 0 = none */
    uint32_t pal;      /* its palette's handle (0 for a texture without one) */
    uint32_t gsCtx;    /* the GS context the packet would have drawn through (for BT3_RND_CHECK) */
    uint64_t gsTex0;   /* the GS fallback: the texture where it was uploaded in GS memory, as TEX0 names it, for one
                          without handles (not converted ahead of time); 0 with tex 0: the colour alone */
    RndState st;
    uint32_t pad;
} RndQuad; /* 112 bytes */

/* port/src/gs_marker.c */
int Port_Quad(const RndQuad *q); /* queues the quad at the display list's cursor; 0 = not taken (the software renderer,
                                    which draws only GS packets: the caller sends its packet as before) */
int Port_TakesRecords(void);     /* 1: Port_Quad takes them now */
void Rnd_StateInit(RndState *s, uint32_t fbp); /* the whole picture, blended normally, no test, nothing else */
int Rnd_Fill(const RndState *st, uint32_t rgba); /* the whole picture in one colour, as Port_Quad */

/* What the GS's two contexts hold while the HUD draws, kept by everything that sets them in the meantime (its
   environment and its parts' mask states; the font's packets through Rnd_DefaultEnv and Rnd_FontEnv).
   gRndCtxValid 0: not known (outside the HUD), the packets are sent. */
extern RndState gRndCtx[2];
extern int gRndCtxValid;
void Rnd_DefaultEnv(uint32_t fbp); /* context 1 as src/sys/gfx.c Gfx_PutDefaultEnv leaves it */
/* What a state packet sets in context ctx (0 = the GS's first): its TEST, FRAME (fbp and write mask: RND_NO_COLOUR,
   RND_NO_ALPHA), FBA, ALPHA (RND_DEST_ALPHA or not), the flags RND_SMOOTH / RND_REPEAT / RND_ZWRITE, SCISSOR. */
void Rnd_SetTest(int ctx, int ztest, int atest, int aref, int afail, int mask);
void Rnd_SetWrite(int ctx, uint32_t fbp, int noWrite);
void Rnd_SetMark(int ctx, int on);
void Rnd_SetBlend(int ctx, int blend, int destAlpha);
void Rnd_SetFlag(int ctx, int flag, int on);
void Rnd_SetScissor(int ctx, int x0, int x1, int y0, int y1);

/* A sprite of a 2D packet whose state gRndCtx[0] holds, while gRndSprites (its owner sets it while it builds the
   packet: src/battle/col_c.c Font_Flush, src/battle/hud_0_b.c BtlMenu_BeginDraw): corners in GS pixels (+ 0x700,
   + 0x720), texels in sixteenths, the texture by handles and / or TEX0. 0 = not taken (gRndSprites 0): the
   caller writes its registers. */
extern int gRndSprites;
int Rnd_Sprite(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, uint32_t rgba, uint32_t tex,
               uint32_t pal, uint64_t tex0);

/* port/src/gs/gs_pvr.c: the handle of the texture record of a 0x40-byte texture entry, of the palette of an
   upload packet; 0 = none (not converted ahead of time, or not uploaded yet). */
uint32_t Port_TexHandle(const void *entry);
uint32_t Port_PalHandle(const void *pkt);

#endif

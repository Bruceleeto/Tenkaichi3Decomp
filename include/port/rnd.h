#ifndef PORT_RND_H
#define PORT_RND_H

/*
 * Drawing without GS packets: what the game hands the renderer itself instead of a GIF packet that sends vertices
 * under the GS's registers. The record goes to the renderer's queue (port/src/gs/gs_walk.c), not into the display
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
 * icons (src/battle/col_c.c, col_c_b.c), the battle menu (src/battle/hud_0_b.c, hud_0_c.c); as polygons (RndPoly,
 * GsGpu_Poly), the effects' draws of the ordering table (src/sys/gfx_ot.c Ot_Send, port/src/rnd_gs.c).
 */

#include <stdint.h>

#define RND_BLEND_NORMAL 0 /* Cs * A + Cd * (1 - A); alpha 0x80 = 1 (A: the source's alpha, or RND_DEST_ALPHA's) */
#define RND_BLEND_ADD 1    /* Cs * A + Cd */
#define RND_BLEND_SUB 2    /* Cd - Cs * A */
#define RND_BLEND_DEST 3   /* Cd * A + Cd (the destination brightened by itself) */
#define RND_BLEND_NONE 4   /* Cs: not blended */

/* RndState.flags */
#define RND_REPEAT 0x01     /* the texture wraps (else it is clamped at its edges) */
#define RND_SMOOTH 0x02     /* filtered */
#define RND_NO_COLOUR 0x04  /* writes the alpha alone */
#define RND_NO_ALPHA 0x08   /* writes the colour alone */
#define RND_DEST_ALPHA 0x10 /* blended by the alpha already in the picture instead of its own */
#define RND_MARK 0x20       /* marks the pixels it writes (bit 7 of their alpha set: the GS's FBA) */
#define RND_ZWRITE 0x40     /* writes its depth */
#define RND_GOURAUD 0x80    /* colours blended between the corners (else each triangle has its last corner's) */
#define RND_ONE 0x100       /* blended by 1 instead of an alpha (the GS's FIX 0x80): add Cs + Cd, subtract Cd - Cs */
#define RND_VU 0x200        /* made by a vertex program (port/src/vu1_native.c; kept by Rnd_Draw): 3D, as the GS's VU1 draws */
#define RND_FIX 0x400       /* with RND_ONE: blended by the constant RndState.fix instead of 1 (the GS's FIX, 0x80 = 1;
                               normal: Cs * fix + Cd * (1 - fix)) */

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
    uint16_t fbp;       /* the frame buffer drawn into (its page) */
    uint8_t fix;        /* RND_FIX's factor (read only with RND_FIX) */
    uint8_t spare;
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

/* A polygon seen in perspective (the effects, src/battle/eft_*.c): each corner its own depth, texture coordinates
   and colour. */
typedef struct RndVtx {
    float x, y;        /* pixels of the 512 x 448 picture */
    uint32_t z;        /* depth, in the 24-bit buffer's steps (bigger is nearer) */
    float s, t, q;     /* texture coordinates times q (0..1 across the texture), and q (1 / w) */
    uint32_t rgba;     /* as RndQuad's */
} RndVtx; /* 28 bytes */

typedef struct RndPoly {
    RndVtx v[4];       /* a triangle (n 3), or a strip of two (n 4: 0 1 2, 1 2 3) */
    uint32_t n;
    uint32_t tex, pal; /* as RndQuad's */
    uint32_t gsCtx;
    uint64_t gsTex0;   /* as RndQuad's; and with tex 0, the native texture uploaded there */
    RndState st;
    uint32_t pad;
} RndPoly; /* 160 bytes */

/* port/src/gs_marker.c */
int Port_Quad(const RndQuad *q); /* queues the quad at the display list's cursor; 0 = not taken (the software renderer,
                                    which draws only GS packets: the caller sends its packet as before) */
int Port_TakesRecords(void);     /* 1: Port_Quad takes them now */
int Port_ListRead(void);         /* 1: the display list is read for what only the PS2 needs (the PS2 reference): 0 = such
                                    packets need not be built (the Dreamcast; BT3_WALK=3 on PC) */
int Port_Poly(const RndPoly *p); /* as Port_Quad */
/* port/src/rnd_gs.c. The GS state the game sets, followed into gRndCtx from each packet the display list gets
   (src/sys/dma.c: DMA CNT tags and their VIF DIRECT data, as Dma_AddData / Dma_EndDirect close them) where the list
   is read (Port_ListRead); elsewhere only looked at (the state put back): the producers set it (Rnd_GsReg). */
int Rnd_GsFollow(const uint32_t *pkt, unsigned bytes, const void *from); /* 1: the packet only sets state, all of it
                                                                           followed; from: its producer (BT3_FOLLOW_LOG) */
void Rnd_GsFollowFrame(void);
/* A GS register's value as the game sets it (address of context 1's register, + 1 for context 2's; PRIM 0x00,
   RGBAQ 0x01, ST 0x02, UV 0x03 too), straight into the renderer's state: a producer's state without a packet. */
void Rnd_GsReg(uint32_t addr, uint64_t v);
/* What of a context's state a record cannot say (gRndCtxBad): a blend that is none of RND_BLEND_, a write mask of
   some channels, a depth test that never passes, a region clamp. */
#define RND_BAD_BLEND 1
#define RND_BAD_FRAME 2
#define RND_BAD_TEST 4
#define RND_BAD_CLAMP 8
extern uint32_t gRndCtxBad[2];
/* A polygon drawn where the display list is, under what the GS's context ctx holds there (gRndCtx), blended (abe)
   or not, its colours blended between the corners (gouraud) or not; its texture by p->tex / p->gsTex0. 0 = not
   taken (the software renderer, or a state gRndCtxBad says): the caller sends its packet. */
int Rnd_Draw(RndPoly *p, int ctx, int abe, int gouraud);
/* A corner from what a GS packet gives: x, y in sixteenths of a pixel (XYOFFSET of context ctx at 0, 0), the
   24-bit depth, s t q, r g b a. */
void Rnd_Vtx(RndVtx *o, int ctx, int32_t x, int32_t y, uint32_t z, float s, float t, float q, uint32_t rgba);
/* The TEX0 of context ctx as the packets followed have set it (a draw whose texture was bound before it). */
uint64_t Rnd_Tex0(int ctx);
/* The PRIM the GS draws by when a packet sets prim: prim itself, or with PRMODECONT.AC 0 (the shadow passes) its type
   with the attributes of PRMODE (texturing, shading, blending, the context). */
uint32_t Rnd_Prim(uint32_t prim);
/* An untextured flat rectangle (a sprite's two corners, as Rnd_Vtx takes them) as Rnd_Draw. */
int Rnd_Rect(int ctx, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t z, uint32_t rgba, int abe);
/* The ordering table's packets as polygons (from src/sys/gfx_ot.c Ot_Send, between Rnd_OtBegin and Rnd_OtEnd):
   whether a packet is a draw taken (1) or one kept (0: its register writes are followed); its polygons to the
   queue; the registers the GS would hold after those taken, written as A+D at dst (the new end) before a packet
   that is kept. */
void Rnd_OtBegin(void);
void Rnd_OtEnd(void);
int Rnd_OtTake(const uint64_t *pkt, unsigned qwc);
void Rnd_OtDraw(const uint64_t *pkt, unsigned qwc);
uint64_t *Rnd_OtState(uint64_t *dst);
/* A draw the effects queue in the ordering table as it is instead of as a GIF packet (src/sys/gfx_ot.c Ot_NewRec):
   what the packet's PRIM says (ctx, abe, gouraud, tme, fst, the type: 1 line, 2 line strip, 3 triangle, 4 strip,
   5 fan, 6 sprite) and its n corners as the packet holds them: x, y in sixteenths of a pixel with XYOFFSET in,
   the 24-bit depth, s t q (with fst: U and V in sixteenths of a texel, as whole numbers in s and t), r g b a (r in
   the low byte). With setTex0, the packet's TEX0 of context ctx is written before it, as the packet wrote it, and
   tex, pal are the handles of the texture it names (0: none; as RndPoly's), which the draws of that context name
   until another TEX0 is written there. */
typedef struct RndOtVtx {
    uint16_t x, y;
    uint32_t z;
    float s, t, q;
    uint32_t rgba;
} RndOtVtx; /* 24 bytes */
typedef struct RndOtRec {
    uint8_t ctx, abe, gouraud, tme, fst, type, n, setTex0;
    uint64_t tex0;
    uint32_t tex, pal; /* (with them 120 bytes) */
    RndOtVtx v[4];
} RndOtRec; /* 112 bytes */
#define RND_OTREC_CLAMP 2 /* setTex0 bit: CLAMP 5 (both clamped) for its corners, then CLAMP 0 (repeat), as the packet */
/* A record of the ordering table: whether it is drawn as polygons (1: Rnd_OtRec, under the state followed to
   that point, as Rnd_OtDraw draws the packet it stands for), or not (0: Rnd_OtRecPacket writes that packet at dst
   and returns its end; its register writes are followed). */
int Rnd_OtRecTakes(const RndOtRec *r);
void Rnd_OtRec(const RndOtRec *r);
uint64_t *Rnd_OtRecPacket(const RndOtRec *r, uint64_t *dst);
extern unsigned gRndOtStat[3]; /* the ordering table's draws sent as packets, as polygons, as records (BT3_GS_VERBOSE) */
void Rnd_StateInit(RndState *s, uint32_t fbp); /* the whole picture, blended normally, no test, nothing else */
int Rnd_Fill(const RndState *st, uint32_t rgba); /* the whole picture in one colour, as Port_Quad */

/* What the GS's two contexts hold, as the display list is made: followed from the packets (Rnd_GsFollow), and set
   by the HUD's environment and its parts' mask states (Rnd_Set*) as well. gRndCtxValid: the HUD takes its sprites
   as records (0 outside the HUD: the packets are sent). */
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
uint32_t Port_PalSub(uint32_t pal, int k); /* palette k (0..3) of 16 in the blocks of a 256 one (the font's four) */

/* port/src/rnd_fx.c: the frame-buffer effects (the glare, the blurs, the haze, the lens, the screen's warp, the
   cross-fade) as records. They draw into a frame buffer by RndState.fbp, and read one as a record's texture. */
int Rnd_FxOn(void); /* 0: no frame buffer but the picture is drawn or read (the Dreamcast; BT3_PVR_NO_RTT=1 on PC): an
                       effect draws nothing then and sends no packet. Tested once at an effect's top. */
/* A frame buffer as a record's texture (RndQuad / RndPoly tex, with pal 0): page fbp read as a w x h texture
   (powers of two, as TEX0's TW / TH; texture coordinates are over that size as for any texture) of pixel format
   psm (the GS's: 0 PSMCT32, 1 PSMCT24; the alpha is the buffer's either way). The renderer takes what the buffer
   holds where the record is drawn. Nothing is drawn while the buffer has never been drawn into, nor by a record
   drawn into the buffer it reads. */
#define RND_TEX_TARGET 0x8000u    /* (a handle of Rnd_TargetTex: the bit no texture record's handle has) */
#define RND_TEX_RGB 0x40000000u   /* or'd into it: the texture's alpha not used, the corners' instead (the GS's
                                     TEX0.TCC 0; with RND_BLEND_ADD / NORMAL the GS's constant ALPHA.FIX as the
                                     corners' alpha) */
uint32_t Rnd_TargetTex(uint32_t fbp, int w, int h, int psm);
/* A rectangle of pixels x0..x1, y0..y1 in one colour, under st (a clear of part of a buffer), as Port_Quad. */
int Rnd_FillRect(const RndState *st, float x0, float y0, float x1, float y1, uint32_t rgba);
/* A sprite under st: pixels x0..x1, y0..y1, texture tex from texel u0, v0 to u1, v1 (sixteenths), its colour
   times rgba (0x80 = 1.0), as Port_Quad. */
int Rnd_Blit(const RndState *st, uint32_t tex, float x0, float y0, float x1, float y1, int u0, int v0, int u1, int v1,
             uint32_t rgba);

#endif

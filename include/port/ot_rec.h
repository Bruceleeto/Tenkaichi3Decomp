#ifndef PORT_OT_REC_H
#define PORT_OT_REC_H

/*
 * The effects' draws queued in the ordering table as they are instead of as GIF packets (src/sys/gfx_ot.c).
 *
 * A record node is an OtPrim with qwc 0 and tagId OT_REC_TAG, followed by the draw itself (OtRec: port/rnd.h
 * RndOtRec) instead of its GIF data. No DMA tag has that upper half (its bits 16..25 are reserved, always 0 in a
 * tag the game writes). It is taken from the packet memory (gOtCur) and linked into a chain as the packet would be;
 * Ot_Send hands it to the renderer (port/src/rnd_gs.c Rnd_OtRec) in its place in the list, which draws it exactly
 * as it would have drawn the packet it stands for.
 *
 * A producer, where Port_TakesRecords(): node = Ot_NewRec(the packet's PRIM, corners), NULL when there is no
 * packet memory; Ot_RecTex0 if the packet writes TEX0 (and Ot_RecTexHandles: the texture's handles); Ot_RecVtx for each corner in the packet's order; then the
 * node linked where the packet would have been. State the packet set by A+D around its draw (CLAMP, TEST) stays
 * a packet: Ot_NewAd nodes linked in the same order around the record.
 */

#include "types.h"
#include "port/rnd.h"

#define OT_REC_TAG 0x03FF
#define OT_REC_HEAD 24 /* the bytes of an OtRec in front of its corners: a node holds only the corners it has */
typedef RndOtRec OtRec;

void *Ot_NewRec(u64 prim, s32 n);
void *Ot_NewAd(u32 addr, u64 data); /* a packet node that writes one GS register (A+D) */
/* Corner i: x, y in sixteenths of a pixel (XYOFFSET in), the 24-bit depth, s t q (with fst: U, V in sixteenths of
   a texel), r g b a (r in the low byte), as the packet holds them. */
void Ot_RecTexHandles(void *node, u32 tex, u32 pal); /* with Ot_RecTex0: the handles of the texture that TEX0 names */
void EftVram_PortRecTex(void *node, u64 tex0); /* src/battle/eft_ae.c: the same for an effect texture, by the TEX0 */
void Ot_RecVtx(void *node, s32 i, s32 x, s32 y, u32 z, f32 s, f32 t, f32 q, u32 rgba);
void Ot_RecTex0(void *node, u64 tex0); /* the TEX0 the packet writes to its context before its corners */
void Ot_RecClamp(void *node); /* its packet writes CLAMP 5 before its corners and CLAMP 0 after (RND_OTREC_CLAMP) */
u32 Ot_RecRgba(f32 r, f32 g, f32 b, f32 a); /* a colour from floats, each converted to a byte as a packet's RGBAQ is */

#endif

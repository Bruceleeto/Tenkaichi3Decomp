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
 * packet memory; Ot_RecTex0 if the packet writes TEX0; Ot_RecVtx for each corner in the packet's order; then the
 * node linked where the packet would have been.
 */

#include "types.h"
#include "port/rnd.h"

#define OT_REC_TAG 0x03FF
typedef RndOtRec OtRec;

void *Ot_NewRec(u64 prim, s32 n);
/* Corner i: x, y in sixteenths of a pixel (XYOFFSET in), the 24-bit depth, s t q (with fst: U, V in sixteenths of
   a texel), r g b a (r in the low byte), as the packet holds them. */
void Ot_RecVtx(void *node, s32 i, s32 x, s32 y, u32 z, f32 s, f32 t, f32 q, u32 rgba);
void Ot_RecTex0(void *node, u64 tex0); /* the TEX0 the packet writes to its context before its corners */
u32 Ot_RecRgba(f32 r, f32 g, f32 b, f32 a); /* a colour from floats, each converted to a byte as a packet's RGBAQ is */

#endif

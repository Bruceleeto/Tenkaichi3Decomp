#ifndef VU1_NATIVE_H
#define VU1_NATIVE_H

/*
 * The VU1 programs as C (port/src/vu1_native.c): a model's or a stage's DMA chain walked where it lies, each batch
 * of vertices run through what the program does to it, and what the program would have sent to the GS handed to
 * the renderer as polygons (include/port/rnd.h, Rnd_Draw) at the display list's cursor, where the call packet
 * would have gone. The constants are the ones the caller would have put in the packets.
 *
 * Chains: the address the call packet would name (a "ret" tag whose VIF codes unpack each batch, V4-32, and start
 * it with MSCNT). A chain of any other shape, a batch the program would not take as it is, or a context whose
 * state a record cannot say: not taken (0), and the caller sends its packet as before.
 */

/* Program 4 (the stage): the 12 quadwords Vu1Pkt_LoadProg4 puts at VU address 0 (screen matrix, clip matrix,
   clip -> screen matrix), kept for the calls that follow. */
void Vu1Native_SetProg4(const void *consts);
/* A mesh's chain drawn by program 4 with them; 1 = taken. */
int Vu1Native_Prog4(const void *chain);

/* Programs 0 (a fighter's part, lit), 1 (its fade pass) and 2 (2a or 2b, as Vu1Pkt_LoadProg2 loaded last: the part
   in one colour, for the shadows): whether the part's chain (part->chain, the part record + 0x60) can be drawn so,
   before its constants are made; then the draw with them (ObjDrawPkt from mtxA, as it would have gone to VU
   address 0: 0x22 quadwords, 0xB for program 2). */
int Vu1Native_Takes(int prog, const void *chain);
void Vu1Native_Prog0(const void *chain, const void *consts);
void Vu1Native_Prog1(const void *chain, const void *consts);
void Vu1Native_Prog2(const void *chain, const void *consts);
/* Program 2's setup: which of the two (alt: 2b) and its constant block at VU address 0 (the 0x13 quadwords of
   Vu1Pkt_LoadProg2; its caller fills 11..18 before the parts are drawn, read then). */
void Vu1Native_SetProg2(int alt, const void *consts);

/* Program 6 (the ground under a fighter, with its shadow page): the 17 quadwords Vu1Pkt_LoadProg6 puts at VU
   address 0 (screen, clip, clip -> screen, shadow camera, scale), kept for the call that follows; the chain
   ObjShadow_BuildPacket made drawn with them, 1 = taken. */
void Vu1Native_SetProg6(const void *consts);
int Vu1Native_Prog6(const void *chain);

/* Program 7 (the stage's debris): a mesh's chain drawn with the 13 quadwords Vu1Pkt_CallProg7 would put at VU
   address 0 (its matrix, screen, clip, its alpha in 12.w); 1 = taken. */
int Vu1Native_Prog7(const void *chain, const void *consts);

/* Program 8 (the stage's animated objects): its constant block at VU address 0 (the 0x12 quadwords of
   Vu1Pkt_LoadProg8; its caller fills screen 10..13 and clip 14..17 before the nodes are drawn, read then); a node's
   chain (node->vif) drawn with the node's 10 quadwords (world, parent, the two offsets: Vu1Node_Draw); 1 = taken. */
void Vu1Native_SetProg8(const void *consts);
int Vu1Native_Prog8(const void *chain, const void *consts);

/* The calls of each program sent as packets [0] and drawn as records [1] (BT3_GS_VERBOSE). */
enum { VU1N_P0, VU1N_P1, VU1N_P4, VU1N_P2A, VU1N_P2B, VU1N_P6, VU1N_P7, VU1N_P8, VU1N_STATS };
extern unsigned gVu1NativeStat[VU1N_STATS][2];

#endif

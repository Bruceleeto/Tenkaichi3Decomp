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

/* Programs 0 (a fighter's part, lit) and 1 (its fade pass): whether the part's chain (part->chain; the part record
   + 0x60 for program 1) can be drawn so, before its constants are made; then the draw with them (the 0x22
   quadwords of ObjDrawPkt from mtxA, as they would have gone to VU address 0). */
int Vu1Native_Takes(int prog, const void *chain);
void Vu1Native_Prog0(const void *chain, const void *consts);
void Vu1Native_Prog1(const void *chain, const void *consts);

#endif

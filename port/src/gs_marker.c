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

/* include/port/rnd.h: the quad as nine writes of register 0x79 and one of 0x7A, which ends it (1: a quad). */
#include <string.h>
#include "../../include/port/rnd.h"

extern int Gs_TakesRecords(void); /* gs/gs_core.c */

typedef char RndQuadSize[sizeof(RndQuad) == 72 ? 1 : -1];

int Port_Quad(const RndQuad *q) {
    uint32_t pkt[4 + 4 + 10 * 4];
    int i;

    if (!Gs_TakesRecords()) {
        return 0;
    }
    pkt[0] = 0x10000000u | 11;          /* DMA CNT, 11 quadwords */
    pkt[1] = 0;
    pkt[2] = 0x10000000u;               /* VIF FLUSHE */
    pkt[3] = 0x50000000u | 11;          /* VIF DIRECT 11 */
    pkt[4] = 0x8000u | 10;              /* GIF tag: 10 loops, EOP, one register: A+D */
    pkt[5] = 0x10000000u;
    pkt[6] = 0xE;
    pkt[7] = 0;
    for (i = 0; i < 10; i++) {
        uint32_t *w = &pkt[8 + i * 4];
        if (i < 9) {
            memcpy(w, (const uint8_t *)q + i * 8, 8);
        } else {
            w[0] = 1;
            w[1] = 0;
        }
        w[2] = i < 9 ? 0x79 : 0x7A;
        w[3] = 0;
    }
    Dma_AddData(pkt, sizeof(pkt));
    return 1;
}

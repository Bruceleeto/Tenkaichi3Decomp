#ifndef PORT_RND_H
#define PORT_RND_H

/*
 * Drawing without GS packets: what the game hands the renderer itself instead of a GIF packet that sets the GS's
 * registers and sends its vertices. The record travels in the display list where the packet would have been (a
 * run of writes to GS registers 0x79 / 0x7A, which the real GS does not have), so it is drawn in the game's order;
 * the GS layer records it as it records a primitive (port/src/gs/gs_draw.c GsGpu_Quad). Nothing of the GS's
 * drawing state is set by it, nor read: the record says all of it.
 * First user: the Flash player's quads (src/sys/gfxm_c.c FlashDraw_Quad).
 */

#include <stdint.h>

#define RND_BLEND_NORMAL 0 /* Cs * As + Cd * (1 - As); alpha 0x80 = 1 */
#define RND_BLEND_ADD 1    /* Cs * As + Cd */
#define RND_BLEND_SUB 2    /* Cd - Cs * As */

#define RND_REPEAT 1 /* the texture wraps (else it is clamped at its edges) */
#define RND_SMOOTH 2 /* filtered when enlarged */

typedef struct RndQuad {
    float xy[4][2];   /* pixels of the 512 x 448 picture, as a strip: top left, top right, bottom left, bottom right */
    int16_t uv[4][2]; /* texels, at the same corners */
    uint32_t rgba;    /* r in the low byte; 0x80 = 1.0 (the texture's colour times this) */
    uint32_t fbp;     /* the frame buffer drawn into (its page) */
    uint64_t tex0;    /* the texture where it was uploaded, as the GS's TEX0 names it; 0 = none, the colour alone */
    uint8_t blend;    /* RND_BLEND_ */
    uint8_t flags;    /* RND_ */
    uint8_t pad[6];
} RndQuad; /* 72 bytes: nine words of the list */

/* port/src/gs_marker.c: puts the quad into the display list. 0 = not taken (the software renderer, which draws only
   GS packets: the caller sends its packet as before). */
int Port_Quad(const RndQuad *q);

#endif

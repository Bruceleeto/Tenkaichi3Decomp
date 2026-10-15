/*
 * The frame-buffer effects as records (include/port/rnd.h): what they draw into a frame buffer other than the
 * picture, and what they read back from one, without GS packets.
 *
 * The PS2 builds its screen effects (the glare, the blurs, the haze, the lens, the warp of the screen, the
 * cross-fade) by drawing the picture into work pages of GS memory and sampling those pages as textures. A record
 * draws into any frame buffer already (RndState.fbp); here a record's texture can be a frame buffer too
 * (Rnd_TargetTex): the renderer takes what that buffer holds where the record is drawn, as it does for a packet
 * that names the page in TEX0 (port/src/gs/gs_draw.c). Nothing is read from GS memory.
 *
 * One switch says whether frame buffers can be drawn into and read at all (Rnd_FxOn): the Dreamcast has no room
 * for them in its video memory, BT3_PVR_NO_RTT=1 turns them off on PC. With it off an effect draws nothing and
 * sends nothing, and the game goes on as if it were not there (port/src/gs/gs_pvr.c leaves the draws into other
 * buffers out by the same switch).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../include/port/rnd.h"

int Rnd_FxOn(void) {
#ifdef __DREAMCAST__
    return 0;
#else
    static int v = -1;
    if (v < 0) { v = getenv("BT3_PVR_NO_RTT") == NULL; }
    return v;
#endif
}

/* log2 of a power of two from 1 to 1024 (else the next power up) */
static uint32_t log2_of(int n) {
    uint32_t k = 0;
    while (k < 10 && (1 << k) < n) { k++; }
    return k;
}

uint32_t Rnd_TargetTex(uint32_t fbp, int w, int h, int psm) {
    return (fbp & 0x1FF) | ((uint32_t)psm & 0x3F) << 9 | RND_TEX_TARGET | log2_of(w) << 16 | log2_of(h) << 20;
}

int Rnd_FillRect(const RndState *st, float x0, float y0, float x1, float y1, uint32_t rgba) {
    RndQuad r;
    int i;

    memset(&r, 0, sizeof(r));
    r.st = *st;
    for (i = 0; i < 4; i++) {
        r.xy[i][0] = (i & 1) ? x1 : x0;
        r.xy[i][1] = (i & 2) ? y1 : y0;
        r.rgba[i] = rgba;
    }
    return Port_Quad(&r);
}

int Rnd_Blit(const RndState *st, uint32_t tex, float x0, float y0, float x1, float y1, int u0, int v0, int u1, int v1,
             uint32_t rgba) {
    RndQuad r;
    int i;

    memset(&r, 0, sizeof(r));
    r.st = *st;
    for (i = 0; i < 4; i++) {
        r.xy[i][0] = (i & 1) ? x1 : x0;
        r.xy[i][1] = (i & 2) ? y1 : y0;
        r.uv[i][0] = (int16_t)((i & 1) ? u1 : u0);
        r.uv[i][1] = (int16_t)((i & 2) ? v1 : v0);
        r.rgba[i] = rgba;
    }
    r.tex = tex;
    return Port_Quad(&r);
}

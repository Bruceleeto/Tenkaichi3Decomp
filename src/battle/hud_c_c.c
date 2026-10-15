#include "common.h"
#include "battle/hud_c.h"
#include "sys/dma.h"
#include "sys/gfx.h"
#include "sys/gfxm_b_b.h"
#include "sys/sprite.h"
#ifdef PORT
#include "port/rnd.h"
#endif

/*
 * Battle HUD: the sprite library, 0x224B50-0x226488 (it ends with HudSprite_Draw and the node setters of
 * src/battle/hud_d.c, 0x226488-0x226500).
 *
 * A HudCSprite is a rectangle in the coordinates of its node. Drawing transforms the four corners by the current
 * VU0 matrix (HudNode_Draw has multiplied the node's translation, rotation and mirror into it), so a sprite is
 * sent as a four-vertex triangle strip, not as a GS sprite. Screen coordinates are pixels of the 512x448 frame;
 * the GS coordinate is (x + 1792) * 16, (y + 1824) * 16. Texture coordinates are texels (sent as texel * 16 + 8).
 *
 * Textures are uploaded on first use in a frame: the image to block `tbp + imageBlock`, the palette to
 * `cbp + clutBlock` (HudSprite_Draw passes 0x2A00 / 0x2C80), and HudCTex.mark keeps a second upload away until
 * Hud_ClearTexMarks.
 *
 * Nothing here reads the pad, the clock or the camera; HudGfx_SetEnv reads the frame counter's parity to pick the
 * colour buffer. No random numbers.
 */

extern void *memset(void *dst, s32 c, u32 n);
extern void Vu0Cur_MulVec4(f32 *out, f32 *v);

#define HUDC_BITBLTBUF(block, width) (((s64)(block) << 32) | ((u64)(width) << 48))

/* Calls fn(0). The HUD parts pass the function that queues a GS state (e.g. HudGauge_GsBeginMask) in front of a
   group of sprites. */
void HudGfx_CallBegin(void (*fn)(s32)) {
    fn(0);
}

/* The same; the parts pass the function that restores the state after the group. */
void HudGfx_CallEnd(void (*fn)(s32)) {
    fn(0);
}

/* Shows or hides a sprite. */
void HudSprite_Show(HudCSprite *spr, s32 show) {
    spr->hidden = show == 0;
}

/* Sets whether the sprite's texture is mirrored left to right. */
void HudSprite_SetMirror(HudCSprite *spr, s32 mirror) {
    spr->mirror = mirror;
}

/* Sets the rectangle (relative to the node). */
void HudSprite_SetRect(HudCSprite *spr, s32 x0, s32 x1, s32 y0, s32 y1) {
    spr->x0 = x0;
    spr->x1 = x1;
    spr->y0 = y0;
    spr->y1 = y1;
}

/* Sets the texel rectangle. */
void HudSprite_SetUv(HudCSprite *spr, s32 u0, s32 u1, s32 v0, s32 v1) {
    spr->u0 = u0;
    spr->u1 = u1;
    spr->v0 = v0;
    spr->v1 = v1;
}

/* Sets the texture entry and the offset of the entry that supplies the palette. */
void HudSprite_SetTex(HudCSprite *spr, s32 tex, s32 sub) {
    spr->tex = tex;
    spr->texSub = sub;
}

/* Sets the colour (0x80 = neutral). */
void HudSprite_SetColor(HudCSprite *spr, s32 r, s32 g, s32 b, s32 a) {
    spr->r = r;
    spr->g = g;
    spr->b = b;
    spr->a = a;
}

/* Makes the sprite an untextured rectangle of the given colour. */
void HudSprite_InitPlain(HudCSprite *spr, u8 r, u8 g, u8 b, u8 a) {
    spr->tex = -1;
    memset(&spr->u0, 0, 8);
    spr->r = r;
    spr->g = g;
    spr->b = b;
    spr->a = a;
}

/* Moves the rectangle. */
void HudSprite_Move(HudCSprite *spr, s32 dx, s32 dy) {
    spr->x0 += dx;
    spr->x1 += dx;
    spr->y0 += dy;
    spr->y1 += dy;
}

/* Moves the texel rectangle. */
void HudSprite_MoveUv(HudCSprite *spr, s32 du, s32 dv) {
    spr->u0 += du;
    spr->u1 += du;
    spr->v0 += dv;
    spr->v1 += dv;
}

/* Exchanges u0 and u1. */
void HudSprite_FlipU(HudCSprite *spr) {
    s16 t = spr->u0;

    spr->u0 = spr->u1;
    spr->u1 = t;
}

/* Exchanges v0 and v1. */
void HudSprite_FlipV(HudCSprite *spr) {
    s16 t = spr->v0;

    spr->v0 = spr->v1;
    spr->v1 = t;
}

/* Scales the rectangle about the node's origin. */
void HudSprite_Scale(HudCSprite *spr, f32 sx, f32 sy) {
    spr->x0 = spr->x0 * sx;
    spr->x1 = spr->x1 * sx;
    spr->y0 = spr->y0 * sy;
    spr->y1 = spr->y1 * sy;
}

/* Moves the rectangle so that its centre is the node's origin. */
void HudSprite_Center(HudCSprite *spr) {
    s32 cx = (spr->x0 + spr->x1) / 2;
    s32 cy = (spr->y0 + spr->y1) / 2;

    spr->x0 -= cx;
    spr->x1 -= cx;
    spr->y0 -= cy;
    spr->y1 -= cy;
}

/* Makes the sprite show the whole of texture `tex` of a sheet: rectangle and texel rectangle (0, w, 0, h) from the
   texture's size in TEX0, neutral colour. */
void HudSprite_InitTex(HudCSprite *spr, HudCRes *res, s32 tex, s32 sub) {
    HudCTex *t = &res->tex[tex];

    if (t != NULL) {
        s32 w = 1 << (s32)((t->tex0 >> 26) & 0xF);
        s32 h = 1 << (s32)((t->tex0 >> 30) & 0xF);

        HudSprite_SetTex(spr, tex, sub);
        HudSprite_SetRect(spr, 0, w, 0, h);
        HudSprite_SetUv(spr, 0, w, 0, h);
        HudSprite_SetColor(spr, 0x80, 0x80, 0x80, 0x80);
    }
}

/* Queues the upload of texture `tex` (image to block tbp + imageBlock, palette to cbp + clutBlock) and of the
   palette of texture `tex + sub`, each only if it has not been uploaded this frame. The palette of `tex + sub`
   goes to the block and width of its own entry but with the palette WIDTH of entry `tex`. */
void HudRes_UploadTex(HudCRes *res, s32 tex, s32 sub, s32 tbp, s32 cbp) {
    u32 pkt[12] = {
        0x10000002, 0x00000000, 0x00000000, 0x50000002, 0x00008001, 0x10000000,
        0x0000000E, 0x00000000, 0x00000000, 0x00000000, 0x00000050, 0x00000000,
    };
    HudCTex *t = &res->tex[tex];
    s32 imageWidth = t->imageWidth;
    s32 clutWidth = t->clutWidth;

    if (!t->mark) {
        t->mark = 1;
        if (t->imageSize != 0) {
            s32 block = t->imageBlock + tbp;

            if (!Port_TexNative(t, block)) {
                pkt[8] = HUDC_BITBLTBUF(block, imageWidth);
                pkt[9] = HUDC_BITBLTBUF(block, imageWidth) >> 32;
                Dma_AddData(pkt, sizeof(pkt));
                Dma_AddRef(t->image, t->imageSize);
            }
        }
        if (t->clutSize != 0) {
            s32 block = t->clutBlock + cbp;

            if (!Port_PalNative(t->clut, t->clutSize, clutWidth, block)) {
                pkt[8] = HUDC_BITBLTBUF(block, clutWidth);
                pkt[9] = HUDC_BITBLTBUF(block, clutWidth) >> 32;
                Dma_AddData(pkt, sizeof(pkt));
                Dma_AddRef(t->clut, t->clutSize);
            }
        }
    }
    t = &res->tex[tex + sub];
    if (!t->mark) {
        t->mark = 1;
        if (t->clutSize != 0) {
            s32 block = t->clutBlock + cbp;

            if (!Port_PalNative(t->clut, t->clutSize, clutWidth, block)) {
                pkt[8] = HUDC_BITBLTBUF(block, clutWidth);
                pkt[9] = HUDC_BITBLTBUF(block, clutWidth) >> 32;
                Dma_AddData(pkt, sizeof(pkt));
                Dma_AddRef(t->clut, t->clutSize);
            }
        }
    }
}

#define HUDC_XYZ(v) \
    ((((s32)(v)[0] << 4) + SPRITE_OFS_X) | ((u64)(((s32)(v)[1] << 4) + SPRITE_OFS_Y) << 16))
#define HUDC_XYZ_CONV(v) \
    ((((s32)(v)[0] << 4) + SPRITE_OFS_X) | ((u64)((Sprite_ConvY((s32)(v)[1]) << 4) + SPRITE_OFS_Y) << 16))

#ifdef PORT
/* The marks where the renderer cannot test them (Rnd_TestsMarks 0: the PVR). The gauges cut a bar to length with
   the GS's destination alpha test: a mask sprite marks the pixels under its texture's opaque texels (context 2
   writing the alpha alone, FBA on), then the bar is drawn where they are not marked (RND_IF_UNMARKED), and
   HudSprite_DrawAlphaClear clears them for the next pass. Here a mask's opaque texels are kept as rectangles of
   pixels and the bar drawn in the pieces of its rectangle outside them, as plain quads. Only sprites drawn straight
   (no rotation) are cut this way; those are all the gauges' (any other is drawn as before: by the mark test). */

/* The masks' opaque texels, as rectangles of texels (u0, v0, u1, v1), read off the disc (pzs3us1/00005.bin's first
   sheet, the gauges': every mask is one of its textures). The test a mask is written with picks which texels count:
   the bars' masks any alpha (TEST_2 0x3000F, alpha not 0), the aura panel's from 0x7F (TEST_1 0x307FB); a slant
   steps every two rows. A mask of a texture not listed is taken as opaque throughout its rectangle. */
/* texture 0x7: the aura panel (HudGauge_DrawAura: sprite 47, alpha from 0x7F) */
static const s16 sHudMaskTex7[][4] = {
    {71, 1, 128, 3}, {69, 3, 129, 5}, {31, 5, 131, 7}, {27, 7, 238, 9}, {25, 9, 236, 11}, {23, 11, 234, 13},
    {21, 13, 233, 15}, {20, 15, 232, 17}, {19, 17, 231, 19}, {19, 19, 229, 21}, {18, 21, 228, 23},
    {17, 23, 118, 25}, {17, 25, 115, 27}, {17, 27, 113, 29}, {17, 29, 112, 31}, {17, 31, 111, 33},
    {17, 33, 110, 35}, {17, 35, 109, 37}, {16, 37, 108, 39}, {15, 39, 107, 41}, {15, 41, 106, 43},
    {15, 43, 104, 45}, {15, 45, 31, 53}, {16, 53, 30, 55}, {18, 55, 28, 57}, {20, 57, 26, 58},
};
/* texture 0x8: the life bars and the blast gauge (HudGauge_DrawHp, DrawBlast) */
static const s16 sHudMaskTex8[][4] = {
    {5, 48, 160, 50}, {4, 50, 159, 52}, {3, 52, 158, 54}, {2, 54, 157, 56}, {1, 56, 156, 58}, {0, 58, 155, 60},
    {194, 0, 218, 2}, {193, 2, 217, 4}, {192, 4, 216, 6},
};
/* texture 0xF: the status icons' shades (HudGauge_DrawAura: sprites 48 to 52, alpha from 0x7F) */
static const s16 sHudMaskTexF[][4] = {
    {5, 12, 40, 14}, {5, 14, 38, 16}, {4, 16, 37, 18}, {4, 18, 36, 20}, {3, 20, 35, 22}, {12, 44, 35, 46},
    {10, 46, 33, 48}, {8, 48, 31, 50}, {7, 50, 30, 52}, {6, 52, 29, 54}, {5, 54, 28, 56}, {4, 56, 27, 58},
    {3, 58, 26, 60}, {2, 60, 25, 62}, {1, 62, 24, 64},
};
/* texture 0x18: the team panel's bars (HudTeam_DrawHpA / DrawHpB / DrawGauge) */
static const s16 sHudMaskTex18[][4] = {
    {1, 16, 30, 18}, {0, 18, 29, 20},
};

typedef struct HudCMaskTex {
    s16 tex, n;
    const s16 (*r)[4];
} HudCMaskTex;

#define HUDC_MASK_TEX(i, a) {i, sizeof(a) / sizeof(a[0]), a}
static const HudCMaskTex sHudMaskTex[] = {
    HUDC_MASK_TEX(7, sHudMaskTex7), HUDC_MASK_TEX(8, sHudMaskTex8), HUDC_MASK_TEX(0xF, sHudMaskTexF), HUDC_MASK_TEX(0x18, sHudMaskTex18),
};

#define HUDC_MARKS 192

static f32 sHudMark[HUDC_MARKS][4]; /* x0, y0, x1, y1 (x0 < x1, y0 < y1): pixels marked */
static s32 sHudMarkCount;

/* Whether the quad of corners v (top left, top right, bottom left, bottom right) is drawn straight. */
static s32 HudSprite_PortStraight(f32 (*v)[4]) {
    return (s32)v[0][1] == (s32)v[1][1] && (s32)v[2][1] == (s32)v[3][1] && (s32)v[0][0] == (s32)v[2][0] &&
           (s32)v[1][0] == (s32)v[3][0] && (s32)v[0][0] != (s32)v[1][0] && (s32)v[0][1] != (s32)v[2][1];
}

/* Marks the pixels under texels u0..u1 x v0..v1 of the sprite whose corners v show texels uv. */
static void HudSprite_PortMarkTexels(f32 (*v)[4], s32 (*uv)[2], s32 u0, s32 v0, s32 u1, s32 v1) {
    f32 xa = (f32)(s32)v[0][0], xb = (f32)(s32)v[1][0], ya = (f32)(s32)v[0][1], yb = (f32)(s32)v[2][1];
    f32 x0 = xa + (f32)(u0 - uv[0][0]) * (xb - xa) / (f32)(uv[1][0] - uv[0][0]);
    f32 x1 = xa + (f32)(u1 - uv[0][0]) * (xb - xa) / (f32)(uv[1][0] - uv[0][0]);
    f32 y0 = ya + (f32)(v0 - uv[0][1]) * (yb - ya) / (f32)(uv[2][1] - uv[0][1]);
    f32 y1 = ya + (f32)(v1 - uv[0][1]) * (yb - ya) / (f32)(uv[2][1] - uv[0][1]);
    f32 *m;

    if (u1 <= u0 || v1 <= v0) {
        return;
    }
    if (sHudMarkCount >= HUDC_MARKS) {
        Port_Lost("a HUD mask's rectangle: no room for another mark (HUDC_MARKS)", HUDC_MARKS, __builtin_return_address(0));
        return;
    }
    m = sHudMark[sHudMarkCount++];
    m[0] = x0 < x1 ? x0 : x1;
    m[2] = x0 < x1 ? x1 : x0;
    m[1] = y0 < y1 ? y0 : y1;
    m[3] = y0 < y1 ? y1 : y0;
}

/* A mask sprite of texture tex: marks the pixels under its opaque texels (sHudMaskTex). */
static void HudSprite_PortMark(f32 (*v)[4], s32 (*uv)[2], s32 tex) {
    s32 u0 = uv[0][0] < uv[1][0] ? uv[0][0] : uv[1][0], u1 = uv[0][0] < uv[1][0] ? uv[1][0] : uv[0][0];
    s32 v0 = uv[0][1] < uv[2][1] ? uv[0][1] : uv[2][1], v1 = uv[0][1] < uv[2][1] ? uv[2][1] : uv[0][1];
    u32 i;
    s32 k;

    if (uv[1][0] == uv[0][0] || uv[2][1] == uv[0][1]) {
        return;
    }
    for (i = 0; i < sizeof(sHudMaskTex) / sizeof(sHudMaskTex[0]); i++) {
        const HudCMaskTex *s = &sHudMaskTex[i];
        if (s->tex == tex) {
            for (k = 0; k < s->n; k++) { /* (each of its rectangles inside the sprite's) */
                HudSprite_PortMarkTexels(v, uv, s->r[k][0] > u0 ? s->r[k][0] : u0, s->r[k][1] > v0 ? s->r[k][1] : v0,
                                         s->r[k][2] < u1 ? s->r[k][2] : u1, s->r[k][3] < v1 ? s->r[k][3] : v1);
            }
            return;
        }
    }
    HudSprite_PortMarkTexels(v, uv, u0, v0, u1, v1);
}

/* The bounds of the quad of corners v: x0, y0, x1, y1. */
static void HudSprite_PortBounds(f32 (*v)[4], f32 *b) {
    s32 k;

    b[0] = b[2] = (f32)(s32)v[0][0];
    b[1] = b[3] = (f32)(s32)v[0][1];
    for (k = 1; k < 4; k++) {
        f32 x = (f32)(s32)v[k][0], y = (f32)(s32)v[k][1];
        b[0] = x < b[0] ? x : b[0];
        b[2] = x > b[2] ? x : b[2];
        b[1] = y < b[1] ? y : b[1];
        b[3] = y > b[3] ? y : b[3];
    }
}

/* Marks (on) or clears the marks of the pixels of rectangle b (x0, y0, x1, y1): a mark it crosses keeps what is
   outside it. */
static void HudSprite_PortMarkRect(const f32 *b, s32 on) {
    s32 i, n = sHudMarkCount;

    for (i = 0; i < n; i++) {
        f32 m[4];
        f32 *d = sHudMark[i];
        if (d[0] >= b[2] || d[2] <= b[0] || d[1] >= b[3] || d[3] <= b[1]) {
            continue;
        }
        m[0] = d[0];
        m[1] = d[1];
        m[2] = d[2];
        m[3] = d[3];
        d[2] = d[0]; /* (emptied: dropped below) */
        if (m[1] < b[1] && sHudMarkCount < HUDC_MARKS) { /* above */
            f32 *e = sHudMark[sHudMarkCount++];
            e[0] = m[0], e[1] = m[1], e[2] = m[2], e[3] = b[1];
        }
        if (m[3] > b[3] && sHudMarkCount < HUDC_MARKS) { /* below */
            f32 *e = sHudMark[sHudMarkCount++];
            e[0] = m[0], e[1] = b[3], e[2] = m[2], e[3] = m[3];
        }
        if (m[0] < b[0] && sHudMarkCount < HUDC_MARKS) { /* left, beside it */
            f32 *e = sHudMark[sHudMarkCount++];
            e[0] = m[0], e[1] = m[1] > b[1] ? m[1] : b[1], e[2] = b[0], e[3] = m[3] < b[3] ? m[3] : b[3];
        }
        if (m[2] > b[2] && sHudMarkCount < HUDC_MARKS) { /* right */
            f32 *e = sHudMark[sHudMarkCount++];
            e[0] = b[2], e[1] = m[1] > b[1] ? m[1] : b[1], e[2] = m[2], e[3] = m[3] < b[3] ? m[3] : b[3];
        }
    }
    for (i = n = 0; i < sHudMarkCount; i++) {
        if (sHudMark[i][2] > sHudMark[i][0]) {
            if (n != i) {
                sHudMark[n][0] = sHudMark[i][0], sHudMark[n][1] = sHudMark[i][1];
                sHudMark[n][2] = sHudMark[i][2], sHudMark[n][3] = sHudMark[i][3];
            }
            n++;
        }
    }
    sHudMarkCount = n;
    if (on && sHudMarkCount < HUDC_MARKS) {
        f32 *e = sHudMark[sHudMarkCount++];
        e[0] = b[0], e[1] = b[1], e[2] = b[2], e[3] = b[3];
    }
}

/* The colour at (x, y) of the quad of corner colours c over x0..x1 x y0..y1, each channel in between. */
static u32 HudSprite_PortLerp(const u32 *c, f32 fx, f32 fy) {
    u32 out = 0;
    s32 k;

    for (k = 0; k < 32; k += 8) {
        f32 top = (f32)((c[0] >> k) & 0xFF) + ((f32)((c[1] >> k) & 0xFF) - (f32)((c[0] >> k) & 0xFF)) * fx;
        f32 bot = (f32)((c[2] >> k) & 0xFF) + ((f32)((c[3] >> k) & 0xFF) - (f32)((c[2] >> k) & 0xFF)) * fx;
        out |= (u32)(s32)(top + (bot - top) * fy + 0.5f) << k;
    }
    return out;
}

/* A sprite drawn where the pixels are not marked (inside 0) or where they are (inside 1): the pieces of its rectangle
   outside the marks or inside them (each pixel once where marks overlap), each a quad r with the texels and colours
   of its place. */
static void HudSprite_PortCut(RndQuad *r, f32 (*v)[4], s32 (*uv)[2], const u32 *rgba, s32 inside) {
    f32 xa = (f32)(s32)v[0][0], xb = (f32)(s32)v[1][0], ya = (f32)(s32)v[0][1], yb = (f32)(s32)v[2][1];
    f32 fx0 = xa < xb ? xa : xb, fx1 = xa < xb ? xb : xa, fy0 = ya < yb ? ya : yb, fy1 = ya < yb ? yb : ya;
    f32 ys[2 * HUDC_MARKS + 2], span[HUDC_MARKS][2];
    s32 ny = 0, i, j, b;

    ys[ny++] = fy0;
    ys[ny++] = fy1;
    for (i = 0; i < sHudMarkCount; i++) {
        for (j = 1; j <= 3; j += 2) {
            if (sHudMark[i][j] > fy0 && sHudMark[i][j] < fy1) {
                ys[ny++] = sHudMark[i][j];
            }
        }
    }
    for (i = 1; i < ny; i++) { /* (in order) */
        f32 y = ys[i];
        for (j = i; j > 0 && ys[j - 1] > y; j--) {
            ys[j] = ys[j - 1];
        }
        ys[j] = y;
    }
    for (b = 0; b + 1 < ny; b++) {
        f32 y0 = ys[b], y1 = ys[b + 1], cur = fx0;
        s32 ns = 0;

        if (y1 <= y0) {
            continue;
        }
        for (i = 0; i < sHudMarkCount; i++) { /* the marks across this band, in order of x */
            f32 *m = sHudMark[i];
            if (m[1] <= y0 && m[3] >= y1 && m[0] < fx1 && m[2] > fx0) {
                for (j = ns; j > 0 && span[j - 1][0] > m[0]; j--) {
                    span[j][0] = span[j - 1][0];
                    span[j][1] = span[j - 1][1];
                }
                span[j][0] = m[0];
                span[j][1] = m[2];
                ns++;
            }
        }
        for (i = 0; i <= ns; i++) {
            f32 x0 = cur, x1 = i < ns ? span[i][0] : fx1;
            if (inside) { /* (the marks' spans, the ones that overlap as one) */
                if (i == ns) {
                    break;
                }
                x0 = span[i][0] > cur ? span[i][0] : cur;
                x1 = span[i][1];
                for (j = i + 1; j < ns && span[j][0] <= x1; j++) {
                    x1 = span[j][1] > x1 ? span[j][1] : x1;
                }
                i = j - 1;
                x0 = x0 > fx0 ? x0 : fx0;
            }
            if (x1 > fx1) {
                x1 = fx1;
            }
            if (x1 > x0) { /* a piece: x0..x1 x y0..y1 */
                RndQuad q = *r;
                f32 px[2], py[2];
                s32 k;
                px[0] = x0;
                px[1] = x1;
                py[0] = y0;
                py[1] = y1;
                for (k = 0; k < 4; k++) {
                    f32 x = px[k & 1], y = py[k >> 1];
                    f32 sx = (x - xa) / (xb - xa), sy = (y - ya) / (yb - ya);
                    q.xy[k][0] = x;
                    q.xy[k][1] = y;
                    q.uv[k][0] = (int16_t)(s32)((f32)((uv[0][0] << 4) + 8) + sx * (f32)((uv[1][0] - uv[0][0]) << 4));
                    q.uv[k][1] = (int16_t)(s32)((f32)((uv[0][1] << 4) + 8) + sy * (f32)((uv[2][1] - uv[0][1]) << 4));
                    q.rgba[k] = HudSprite_PortLerp(rgba, sx, sy);
                }
                Port_Quad(&q);
            }
            if (inside) {
                cur = x1 > cur ? x1 : cur;
            } else if (i < ns && span[i][1] > cur) {
                cur = span[i][1];
            }
        }
    }
}

/* The sprite as a record (include/port/rnd.h) instead of its vertices: the corners at the whole pixels the packet
   gives them, the texels with its half-texel offset, a colour per corner, and the state of GS context ctx as the
   HUD has set it (gRndCtx). t: the texture entry, pal: the one whose palette it is drawn with, tex0 the TEX0 the
   packet sends. `end`: the end of the packet begun for it (its state, ended in front of the record). 0 = not
   taken: the vertices are sent. */
static s32 HudSprite_PortQuad(s32 ctx, f32 (*v)[4], s32 (*uv)[2], HudCTex *t, HudCTex *pal, u64 tex0, const u32 *rgba, u64 *end, s32 texIdx) {
    RndQuad r;
    s32 i;

    if (!Port_TakesRecords()) {
        return 0;
    }
    if (!gRndCtxValid) {
        Port_Lost("a HUD sprite drawn before the HUD's state (gRndCtxValid 0)", (u32)ctx, __builtin_return_address(0));
        return 0;
    }
    Dma_EndDirect(end);
    memset(&r, 0, sizeof(r));
    r.st = gRndCtx[ctx];
    for (i = 0; i < 4; i++) {
        r.xy[i][0] = (f32)(s32)v[i][0];
        r.xy[i][1] = (f32)(s32)v[i][1];
        r.rgba[i] = rgba[i];
        if (uv != NULL) {
            r.uv[i][0] = (int16_t)((uv[i][0] << 4) + 8);
            r.uv[i][1] = (int16_t)((uv[i][1] << 4) + 8);
        }
    }
    if (t != NULL) {
        r.tex = Port_TexHandle(t);
        r.pal = pal->clutSize != 0 ? Port_PalHandle(pal->clut) : 0;
        r.gsTex0 = tex0;
    }
    r.gsCtx = (u32)ctx;
    if (!Rnd_TestsMarks() && HudSprite_PortStraight(v)) {
        if ((r.st.flags & RND_MARK) && (r.st.flags & RND_NO_COLOUR) && r.st.mask == RND_MASK_NONE) {
            if (uv != NULL) { /* a mask (it writes nothing else) */
                HudSprite_PortMark(v, uv, texIdx);
                return 1;
            }
        } else if (r.st.mask == RND_IF_UNMARKED || r.st.mask == RND_IF_MARKED) {
            s32 none[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            s32 inside = r.st.mask == RND_IF_MARKED;
            r.st.mask = RND_MASK_NONE;
            HudSprite_PortCut(&r, v, uv != NULL ? uv : none, rgba, inside);
            return 1;
        }
    } else if (!Rnd_TestsMarks() && r.st.mask != RND_MASK_NONE) {
        Port_Lost("a HUD sprite drawn by the marks, turned (the cut takes straight ones)", (u32)r.st.mask, __builtin_return_address(0));
    }
    return Port_Quad(&r);
}
#endif

/* Draws the sprite's rectangle with colour 0, alpha 0 (source alpha blend, no texture, no tests): with the
   blend equation in force this leaves the picture and clears the destination alpha, which the gauge part uses as
   a mask. */
void HudSprite_DrawAlphaClear(HudCSprite *spr) {
    f32 v[4][4];
    u64 *p;

    v[0][0] = spr->x0;
    v[0][1] = spr->y0;
    v[0][3] = 1.0f;
    v[1][0] = spr->x1;
    v[1][1] = spr->y0;
    v[1][3] = 1.0f;
    v[2][0] = spr->x0;
    v[2][1] = spr->y1;
    v[2][3] = 1.0f;
    v[3][0] = spr->x1;
    v[3][1] = spr->y1;
    v[3][3] = 1.0f;
    Vu0Cur_MulVec4(v[0], v[0]);
    Vu0Cur_MulVec4(v[1], v[1]);
    Vu0Cur_MulVec4(v[2], v[2]);
    Vu0Cur_MulVec4(v[3], v[3]);
    v[0][1] = Sprite_ConvY(v[0][1]);
    v[1][1] = Sprite_ConvY(v[1][1]);
    v[2][1] = Sprite_ConvY(v[2][1]);
    v[3][1] = Sprite_ConvY(v[3][1]);
    p = Dma_BeginDirect();
    PORT_PKT(p, 0, GIF_TAG(5, 0, 1), GIF_REG_AD);
    p += 2;
    PORT_REG(p, 0, GS_ALPHA_1, 0x44);
    p += 2;
    PORT_REG(p, 0, GS_TEST_1, 0x30000); /* alpha and depth tests always pass */
    p += 2;
    PORT_REG(p, 0, GS_FBA_1, 0);
    p += 2;
    PORT_REG(p, 0, GS_RGBAQ, GFX_CLEAR_RGBAQ);
    p += 2;
    PORT_REG(p, 0, GS_PRIM, 0x44); /* triangle strip, alpha blended */
    p += 2;
#ifdef PORT
    Rnd_SetBlend(0, RND_BLEND_NORMAL, 0);                                /* ALPHA_1 0x44 */
    Rnd_SetTest(0, RND_Z_ALWAYS, RND_ATEST_ALWAYS, 0, 0, RND_MASK_NONE); /* TEST_1 0x30000 */
    Rnd_SetMark(0, 0);                                                  /* FBA_1 0 */
    {
        u32 c[4] = {(u32)GFX_CLEAR_RGBAQ, (u32)GFX_CLEAR_RGBAQ, (u32)GFX_CLEAR_RGBAQ, (u32)GFX_CLEAR_RGBAQ};
        f32 b[4];
        HudSprite_PortBounds(v, b);
        HudSprite_PortMarkRect(b, 0); /* (its alpha 0: the marks there cleared) */
        p[-12] |= 1ull << 15; /* (the A+D tag's EOP: the end of the packet if the record is taken) */
        if (HudSprite_PortQuad(0, v, NULL, NULL, NULL, 0, c, p, -1)) {
            return;
        }
        p[-12] &= ~(1ull << 15);
    }
#endif
    PORT_PKT(p, 0, GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 4), 0x5555);
    p += 2;
    p[0] = HUDC_XYZ_CONV(v[0]);
    p[1] = HUDC_XYZ_CONV(v[1]);
    p += 2;
    p[0] = HUDC_XYZ_CONV(v[2]);
    p[1] = HUDC_XYZ_CONV(v[3]);
    p += 2;
    Dma_EndDirect(p);
}

/* The same with alpha 0xFF. */
void HudSprite_DrawAlphaSet(HudCSprite *spr) {
    f32 v[4][4];
    u64 *p;

    v[0][0] = spr->x0;
    v[0][1] = spr->y0;
    v[0][3] = 1.0f;
    v[1][0] = spr->x1;
    v[1][1] = spr->y0;
    v[1][3] = 1.0f;
    v[2][0] = spr->x0;
    v[2][1] = spr->y1;
    v[2][3] = 1.0f;
    v[3][0] = spr->x1;
    v[3][1] = spr->y1;
    v[3][3] = 1.0f;
    Vu0Cur_MulVec4(v[0], v[0]);
    Vu0Cur_MulVec4(v[1], v[1]);
    Vu0Cur_MulVec4(v[2], v[2]);
    Vu0Cur_MulVec4(v[3], v[3]);
    v[0][1] = Sprite_ConvY(v[0][1]);
    v[1][1] = Sprite_ConvY(v[1][1]);
    v[2][1] = Sprite_ConvY(v[2][1]);
    v[3][1] = Sprite_ConvY(v[3][1]);
    p = Dma_BeginDirect();
    PORT_PKT(p, 0, GIF_TAG(5, 0, 1), GIF_REG_AD);
    p += 2;
    PORT_REG(p, 0, GS_ALPHA_1, 0x44);
    p += 2;
    PORT_REG(p, 0, GS_TEST_1, 0x30000);
    p += 2;
    PORT_REG(p, 0, GS_FBA_1, 0);
    p += 2;
    PORT_PKT(p, 0, 0x3F800000FF000000, GS_RGBAQ);
    p += 2;
    PORT_REG(p, 0, GS_PRIM, 0x44);
    p += 2;
#ifdef PORT
    Rnd_SetBlend(0, RND_BLEND_NORMAL, 0);                                /* ALPHA_1 0x44 */
    Rnd_SetTest(0, RND_Z_ALWAYS, RND_ATEST_ALWAYS, 0, 0, RND_MASK_NONE); /* TEST_1 0x30000 */
    Rnd_SetMark(0, 0);                                                  /* FBA_1 0 */
    {
        u32 c[4] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
        f32 b[4];
        HudSprite_PortBounds(v, b);
        HudSprite_PortMarkRect(b, 1); /* (its alpha 0xFF: the pixels marked) */
        p[-12] |= 1ull << 15; /* (the A+D tag's EOP: the end of the packet if the record is taken) */
        if (HudSprite_PortQuad(0, v, NULL, NULL, NULL, 0, c, p, -1)) {
            return;
        }
        p[-12] &= ~(1ull << 15);
    }
#endif
    PORT_PKT(p, 0, GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 4), 0x5555);
    p += 2;
    p[0] = HUDC_XYZ_CONV(v[0]);
    p[1] = HUDC_XYZ_CONV(v[1]);
    p += 2;
    p[0] = HUDC_XYZ_CONV(v[2]);
    p[1] = HUDC_XYZ_CONV(v[3]);
    p += 2;
    Dma_EndDirect(p);
}

/* Fills the frame with colour 0, alpha 0 in 16 strips of 32 x 448 (blended sprites, depth writes on), then sets
   the neutral colour and turns depth writes off. Hud_Draw calls it in front of each side's gauges. */
void HudGfx_ClearAlpha(void) {
    u64 *p = Dma_BeginDirect();
    s32 i;
    s32 n;
    u64 *tag;

#ifdef PORT
    if (gRndCtxValid && Port_TakesRecords()) {
        /* The fill as a record between the state it is drawn in and the state after it: the whole picture, colour 0,
           alpha 0, under context 1's state with its depth (0) written. */
        static f32 v[4][4] = {{0, 0}, {GFX_WIDTH, 0}, {0, GFX_HEIGHT}, {GFX_WIDTH, GFX_HEIGHT}};
        u32 c[4] = {0, 0, 0, 0};
        RndState keep = gRndCtx[0];

        sHudMarkCount = 0; /* (the alpha of the whole picture cleared) */

        PORT_PKT(p, 0, GIF_TAG(2, 1, 1), GIF_REG_AD);
        p += 2;
        PORT_REG(p, 0, GS_RGBAQ, GFX_CLEAR_RGBAQ);
        p += 2;
        PORT_REG(p, 0, GS_ZBUF_1, GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0));
        p += 2;
        Rnd_SetFlag(0, RND_ZWRITE, 1);
        HudSprite_PortQuad(0, v, NULL, NULL, NULL, 0, c, p, -1);
        gRndCtx[0] = keep;
        p = Dma_BeginDirect();
        PORT_PKT(p, 0, GIF_TAG(2, 1, 1), GIF_REG_AD);
        p += 2;
        PORT_PKT(p, 0, 0x3F80000080808080, GS_RGBAQ);
        p += 2;
        PORT_REG(p, 0, GS_ZBUF_1, GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 1));
        p += 2;
        Dma_EndDirect(p);
        return;
    }
#endif

    PORT_PKT(p, 0, GIF_TAG(3, 0, 1), GIF_REG_AD);
    p += 2;
    PORT_REG(p, 0, GS_RGBAQ, GFX_CLEAR_RGBAQ);
    p += 2;
    PORT_REG(p, 0, GS_ZBUF_1, GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 0));
    p += 2;
    PORT_REG(p, 0, GS_PRIM, 0x46); /* sprite, alpha blended */
    p += 2;
    tag = p;
    p += 2;
    i = 0;
    for (n = 0; n < 16; n++) {
        p[0] = ((i * 32 + 1792) << 4) | ((1824 << 4) << 16);
        p[1] = GS_SET_XYZ((i * 32 + 1792 + 32) << 4, (1824 + 448) << 4, 0);
        p += 2;
        i++;
    }
    PORT_PKT(tag, 0, GIF_TAG_EX(16, 1, GIF_FLG_REGLIST, 2), GS_REG_XYZ2 | (GS_REG_XYZ2 << 4));
    PORT_PKT(p, 0, GIF_TAG(2, 0, 1), GIF_REG_AD);
    p += 2;
    PORT_PKT(p, 0, 0x3F80000080808080, GS_RGBAQ);
    p += 2;
    PORT_REG(p, 0, GS_ZBUF_1, GS_SET_ZBUF(GFX_ZBP, GS_PSMZ24, 1));
    p += 2;
    Dma_EndDirect(p);
}

#define HUDC_FRAME_WORD() (!(gGfx.frame & 1) ? 0x80070 : 0x80000)

/* Queues the HUD's drawing state, the same for GS contexts 1 and 2: the frame's colour buffer, the depth buffer
   with writes off, the full-screen offset and scissor, source alpha blending, tests that always pass, bilinear
   filtering, no FBA, repeating textures, colour clamp and PRMODECONT. Called at the top of Hud_Draw. */
void HudGfx_SetEnv(void) {
    u32 pkt[88] = {
        0x10000015, 0, 0x10000000, 0x50000015,
        0x00008014, 0x10000000, GIF_REG_AD, 0,
        HUDC_FRAME_WORD(), 0, GS_FRAME_1, 0,
        HUDC_FRAME_WORD(), 0, GS_FRAME_2, 0,
        0x310000E0, 1, GS_ZBUF_1, 0,
        0x310000E0, 1, GS_ZBUF_2, 0,
        GFX_OFX, GFX_OFY, GS_XYOFFSET_1, 0,
        GFX_OFX, GFX_OFY, GS_XYOFFSET_2, 0,
        0x01FF0000, 0x01BF0000, GS_SCISSOR_1, 0,
        0x01FF0000, 0x01BF0000, GS_SCISSOR_2, 0,
        0x44, 0, GS_ALPHA_1, 0,
        0x44, 0, GS_ALPHA_2, 0,
        0x30000, 0, GS_TEST_1, 0,
        0x30000, 0, GS_TEST_2, 0,
        0x60, 0, GS_TEX1_1, 0,
        0x60, 0, 0x15, 0,
        0, 0, GS_FBA_1, 0,
        0, 0, GS_FBA_2, 0,
        0, 0, GS_CLAMP_1, 0,
        0, 0, GS_CLAMP_2, 0,
        1, 0, GS_COLCLAMP, 0,
        1, 0, GS_PRMODECONT, 0,
    };

#ifdef PORT
    Rnd_GsReg(GS_FRAME_1, (u32)(HUDC_FRAME_WORD()));
    Rnd_GsReg(GS_FRAME_2, (u32)(HUDC_FRAME_WORD()));
    Rnd_GsReg(GS_ZBUF_1, ((u64)(u32)(1) << 32 | (u32)(0x310000E0)));
    Rnd_GsReg(GS_ZBUF_2, ((u64)(u32)(1) << 32 | (u32)(0x310000E0)));
    Rnd_GsReg(GS_XYOFFSET_1, ((u64)(u32)(GFX_OFY) << 32 | (u32)(GFX_OFX)));
    Rnd_GsReg(GS_XYOFFSET_2, ((u64)(u32)(GFX_OFY) << 32 | (u32)(GFX_OFX)));
    Rnd_GsReg(GS_SCISSOR_1, ((u64)(u32)(0x01BF0000) << 32 | (u32)(0x01FF0000)));
    Rnd_GsReg(GS_SCISSOR_2, ((u64)(u32)(0x01BF0000) << 32 | (u32)(0x01FF0000)));
    Rnd_GsReg(GS_ALPHA_1, 0x44);
    Rnd_GsReg(GS_ALPHA_2, 0x44);
    Rnd_GsReg(GS_TEST_1, 0x30000);
    Rnd_GsReg(GS_TEST_2, 0x30000);
    Rnd_GsReg(GS_TEX1_1, 0x60);
    Rnd_GsReg(0x15, 0x60);
    Rnd_GsReg(GS_FBA_1, 0);
    Rnd_GsReg(GS_FBA_2, 0);
    Rnd_GsReg(GS_CLAMP_1, 0);
    Rnd_GsReg(GS_CLAMP_2, 0);
    Rnd_GsReg(GS_COLCLAMP, 1);
    Rnd_GsReg(GS_PRMODECONT, 1);
    if (Port_ListRead()) /* (the packet only for the PS2 reference) */
#endif
    Dma_AddData(pkt, sizeof(pkt));
#ifdef PORT
    {   /* both contexts: the frame, depth not written, the whole picture, blended normally, no test, filtered,
           repeating */
        s32 i;
        for (i = 0; i < 2; i++) {
            Rnd_StateInit(&gRndCtx[i], (u32)(HUDC_FRAME_WORD() & 0x1FF));
            gRndCtx[i].flags = RND_SMOOTH | RND_REPEAT;
        }
        gRndCtxValid = 1;
    }
#endif
}

/* Draws a sprite as a four-vertex triangle strip under the current VU0 matrix. A textured sprite uploads its
   texture (HudRes_UploadTex with tbp / cbp) and sets TEX0 of both GS contexts; `ctx2` draws through context 2. */
void HudSprite_DrawAt(HudCSprite *spr, HudCRes *res, u8 ctx2, s32 tbp, s32 cbp) {
    f32 v[4][4];
    s32 uv[4][2];
    HudCTex *t = NULL;
    HudCTex *pal = NULL;
    u64 *p;
    u64 *tag;
    s32 ctx = ctx2 != 0;

    if (spr->flags & HUDC_SPR_HIDDEN) {
        return;
    }
    memset(v, 0, sizeof(v));
    if (spr->tex >= 0) {
        HudRes_UploadTex(res, spr->tex, spr->texSub, tbp, cbp);
        t = &res->tex[spr->tex];
        pal = &res->tex[spr->tex + spr->texSub];
    }
    p = Dma_BeginDirect();
    tag = p;
    p += 2;
    v[0][0] = spr->x0;
    v[0][1] = spr->y0;
    v[0][3] = 1.0f;
    v[1][0] = spr->x1;
    v[1][1] = spr->y0;
    v[1][3] = 1.0f;
    v[2][0] = spr->x0;
    v[2][1] = spr->y1;
    v[2][3] = 1.0f;
    v[3][0] = spr->x1;
    v[3][1] = spr->y1;
    v[3][3] = 1.0f;
    Vu0Cur_MulVec4(v[0], v[0]);
    Vu0Cur_MulVec4(v[1], v[1]);
    Vu0Cur_MulVec4(v[2], v[2]);
    Vu0Cur_MulVec4(v[3], v[3]);
    v[0][1] = Sprite_ConvY(v[0][1]);
    v[1][1] = Sprite_ConvY(v[1][1]);
    v[2][1] = Sprite_ConvY(v[2][1]);
    v[3][1] = Sprite_ConvY(v[3][1]);
    if (spr->flags & HUDC_SPR_MIRROR) {
        uv[0][0] = spr->u1;
        uv[2][0] = spr->u1;
        uv[1][0] = spr->u0;
        uv[3][0] = spr->u0;
        uv[0][1] = spr->v0;
        uv[1][1] = spr->v0;
        uv[2][1] = spr->v1;
        uv[3][1] = spr->v1;
    } else {
        uv[0][0] = spr->u0;
        uv[2][0] = spr->u0;
        uv[1][0] = spr->u1;
        uv[3][0] = spr->u1;
        uv[0][1] = spr->v0;
        uv[1][1] = spr->v0;
        uv[2][1] = spr->v1;
        uv[3][1] = spr->v1;
    }
#ifdef PORT
    {
        u32 c = (u32)spr->r | ((u32)spr->g << 8) | ((u32)spr->b << 16) | ((u32)spr->a << 24);
        u32 cs[4] = {c, c, c, c};
        u64 t0 = spr->tex >= 0 ? t->tex0 | ((u64)(pal->clutBlock + cbp) << 37) | (t->imageBlock + tbp) | ((u64)0x8000 << 19) : 0;
        if (HudSprite_PortQuad(ctx, v, uv, spr->tex >= 0 ? t : NULL, pal, t0, cs, tag, spr->tex)) {
            return;
        }
    }
#endif
    if (spr->tex >= 0) {
        s32 block = t->imageBlock + tbp;
        u64 clut = (u64)(pal->clutBlock + cbp) << 37;

        p[0] = (u64)spr->r | ((u64)spr->g << 8) | ((u64)spr->b << 16) | ((u64)spr->a << 24);
        p[1] = t->tex0 | clut | block | ((u64)0x8000 << 19);
        p += 2;
        p[0] = t->tex0 | clut | block | ((u64)0x8000 << 19);
        p[1] = ((u64)ctx << 9) | 0x154; /* triangle strip, textured, alpha blended, UV coordinates */
        p += 2;
    } else {
        p[0] = (u64)spr->r | ((u64)spr->g << 8) | ((u64)spr->b << 16) | ((u64)spr->a << 24);
        p[1] = ((u64)ctx << 9) | 0x44;
        p += 2;
    }
    p[0] = ((uv[0][0] << 4) + 8) | ((u64)((uv[0][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[0]);
    p += 2;
    p[0] = ((uv[1][0] << 4) + 8) | ((u64)((uv[1][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[1]);
    p += 2;
    p[0] = ((uv[2][0] << 4) + 8) | ((u64)((uv[2][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[2]);
    p += 2;
    p[0] = ((uv[3][0] << 4) + 8) | ((u64)((uv[3][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[3]);
    p += 2;
    if (spr->tex >= 0) {
        tag[0] = GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 12);
        tag[1] = 0x535353530671; /* RGBAQ, TEX0_2, TEX0_1, PRIM, 4 x (UV, XYZ2) */
    } else {
        tag[0] = GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 10);
        tag[1] = 0x5353535301; /* RGBAQ, PRIM, 4 x (UV, XYZ2) */
    }
    Dma_EndDirect(p);
}

#define HUDC_RGBA(c) \
    ((u64)(((c) >> 24) & 0xFF) | ((u64)(((c) >> 16) & 0xFF) << 8) | ((u64)(((c) >> 8) & 0xFF) << 16) | \
     ((u64)((c) & 0xFF) << 24))

/* Draws a sprite like HudSprite_DrawAt (textures at HUDC_GRADIENT_TBP / CBP), but with a colour per vertex instead
   of the sprite's own: colorL on the two left corners, colorR on the two right ones, each 0xRRGGBBAA. */
void HudSprite_DrawGradient(HudCSprite *spr, HudCRes *res, u8 ctx2, s32 colorL, s32 colorR) {
    f32 v[4][4];
    s32 uv[4][2];
    HudCTex *t = NULL;
    HudCTex *pal = NULL;
    u64 *p;
    u64 *tag;
    s32 ctx = ctx2 != 0;

    if (spr->flags & HUDC_SPR_HIDDEN) {
        return;
    }
    memset(v, 0, sizeof(v));
    if (spr->tex >= 0) {
        HudRes_UploadTex(res, spr->tex, spr->texSub, HUDC_GRADIENT_TBP, HUDC_GRADIENT_CBP);
        t = &res->tex[spr->tex];
        pal = &res->tex[spr->tex + spr->texSub];
    }
    p = Dma_BeginDirect();
    tag = p;
    p += 2;
    v[0][0] = spr->x0;
    v[0][1] = spr->y0;
    v[0][3] = 1.0f;
    v[1][0] = spr->x1;
    v[1][1] = spr->y0;
    v[1][3] = 1.0f;
    v[2][0] = spr->x0;
    v[2][1] = spr->y1;
    v[2][3] = 1.0f;
    v[3][0] = spr->x1;
    v[3][1] = spr->y1;
    v[3][3] = 1.0f;
    Vu0Cur_MulVec4(v[0], v[0]);
    Vu0Cur_MulVec4(v[1], v[1]);
    Vu0Cur_MulVec4(v[2], v[2]);
    Vu0Cur_MulVec4(v[3], v[3]);
    v[0][1] = Sprite_ConvY(v[0][1]);
    v[1][1] = Sprite_ConvY(v[1][1]);
    v[2][1] = Sprite_ConvY(v[2][1]);
    v[3][1] = Sprite_ConvY(v[3][1]);
    if (spr->flags & HUDC_SPR_MIRROR) {
        uv[0][0] = spr->u1;
        uv[2][0] = spr->u1;
        uv[1][0] = spr->u0;
        uv[3][0] = spr->u0;
        uv[0][1] = spr->v0;
        uv[1][1] = spr->v0;
        uv[2][1] = spr->v1;
        uv[3][1] = spr->v1;
    } else {
        uv[0][0] = spr->u0;
        uv[2][0] = spr->u0;
        uv[1][0] = spr->u1;
        uv[3][0] = spr->u1;
        uv[0][1] = spr->v0;
        uv[1][1] = spr->v0;
        uv[2][1] = spr->v1;
        uv[3][1] = spr->v1;
    }
#ifdef PORT
    {
        u32 cl = (u32)HUDC_RGBA(colorL), cr = (u32)HUDC_RGBA(colorR);
        u32 cs[4] = {cl, cr, cl, cr};
        u64 t0 = spr->tex >= 0 ? t->tex0 | ((u64)(pal->clutBlock + HUDC_GRADIENT_CBP) << 37) | (t->imageBlock + HUDC_GRADIENT_TBP) | ((u64)0x8000 << 19) : 0;
        if (HudSprite_PortQuad(ctx, v, uv, spr->tex >= 0 ? t : NULL, pal, t0, cs, tag, spr->tex)) {
            return;
        }
    }
#endif
    if (spr->tex >= 0) {
        s32 block = t->imageBlock + HUDC_GRADIENT_TBP;
        u64 clut = (u64)(pal->clutBlock + HUDC_GRADIENT_CBP) << 37;

        p[0] = t->tex0 | clut | block | ((u64)0x8000 << 19);
        p[1] = t->tex0 | clut | block | ((u64)0x8000 << 19);
        p += 2;
        p[0] = ((u64)ctx << 9) | 0x154;
        p[1] = HUDC_RGBA(colorL);
        p += 2;
    } else {
        p[0] = ((u64)ctx << 9) | 0x44;
        p[1] = HUDC_RGBA(colorL);
        p += 2;
    }
    p[0] = ((uv[0][0] << 4) + 8) | ((u64)((uv[0][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[0]);
    p += 2;
    p[0] = HUDC_RGBA(colorR);
    p[1] = ((uv[1][0] << 4) + 8) | ((u64)((uv[1][1] << 4) + 8) << 16);
    p += 2;
    p[0] = HUDC_XYZ(v[1]);
    p[1] = HUDC_RGBA(colorL);
    p += 2;
    p[0] = ((uv[2][0] << 4) + 8) | ((u64)((uv[2][1] << 4) + 8) << 16);
    p[1] = HUDC_XYZ(v[2]);
    p += 2;
    p[0] = HUDC_RGBA(colorR);
    p[1] = ((uv[3][0] << 4) + 8) | ((u64)((uv[3][1] << 4) + 8) << 16);
    p += 2;
    p[0] = HUDC_XYZ(v[3]);
    p[1] = 0;
    p += 2;
    if (spr->tex >= 0) {
        tag[0] = GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 15);
        tag[1] = 0x0531531531531067; /* TEX0_2, TEX0_1, PRIM, 4 x (RGBAQ, UV, XYZ2) */
    } else {
        tag[0] = GIF_TAG_EX(1, 1, GIF_FLG_REGLIST, 13);
        tag[1] = 0x5315315315310; /* PRIM, 4 x (RGBAQ, UV, XYZ2) */
    }
    Dma_EndDirect(p);
}

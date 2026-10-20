/* kos_single_vbuf.h — give a single-buffered KOS PVR setup one vertex buffer as large as a whole
 * VRAM half, without changing texture memory. See README.md next to this file.
 *
 * Usage (include in exactly one .c file):
 *
 *     pvr_init_params_t params = {
 *         { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_32 },
 *         1792 * 1024,   // vertex_buf_size: only sets how much VRAM KOS reserves below textures
 *         0, 0,
 *         1,             // autosort_disabled -> pass the same value as `presort` below
 *         8,             // opb_overflow_count
 *         1              // vbuf_doublebuf_disabled: REQUIRED
 *     };
 *     pvr_init(&params);
 *     uint32_t vbuf = kos_single_vbuf_layout(true);   // 0 = layout left as KOS made it
 *     // size your own TA budget from vbuf (or from vertex_buf_size if it returned 0)
 *
 * Checked against KallistiOS 7d0972e3 and master (pvr_buffers.c's two-set loop, pvr_misc.c's
 * register sync and render start, pvr_irq.c's buffer/page flips). It reaches into KOS internals
 * (pvr_internal.h): re-check it when KOS changes those files.
 */
#ifndef KOS_SINGLE_VBUF_H
#define KOS_SINGLE_VBUF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <dc/pvr.h>
#include <../kernel/arch/dreamcast/hardware/pvr/pvr_internal.h>   /* pvr_state, pvr_ta_buffers_t */

#define KSV_VRAM_HALF 0x400000u                  /* 32-bit VRAM area: two 4 MB halves */
#ifndef PVR_TILE_MATRIX_HEADER_SIZE
#define PVR_TILE_MATRIX_HEADER_SIZE 0x48         /* private to pvr_buffers.c */
#endif

static inline uint32_t ksv_align128(uint32_t v) { return (v + 127u) & ~127u; }

/* Call once, right after pvr_init() and before any texture allocation or scene.
 * presort must equal params.autosort_disabled != 0.
 * Returns the new vertex buffer size in bytes, or 0 if nothing was changed. */
static uint32_t kos_single_vbuf_layout(bool presort) {
    volatile pvr_ta_buffers_t *b0 = &pvr_state.ta_buffers[0];
    if (pvr_state.vbuf_doublebuf) return 0;       /* only valid with a single vertex buffer */

    /* KOS reserved [0, h) of each half for its two identical sets; textures start above that
     * in both halves (64-bit texture offset T sits at 32-bit offset T/2 of each half). */
    uint32_t h = pvr_state.texture_base / 2u;
    uint32_t opb_total = b0->opb_size, ovf = b0->opb_overflow_count;
    uint32_t fb0 = pvr_state.frame_buffers[0].frame_size, fb1 = pvr_state.frame_buffers[1].frame_size;
    uint32_t tm = b0->tile_matrix_size;

    /* Half 1: both framebuffers, the OPBs with their overflow sets, the tile matrix. */
    uint32_t a = KSV_VRAM_HALF;
    uint32_t f0 = a;   a = ksv_align128(a + fb0);
    uint32_t f1 = a;   a = ksv_align128(a + fb1);
    uint32_t opb = a;  a = ksv_align128(a + opb_total * (1u + ovf));
    a += PVR_TILE_MATRIX_HEADER_SIZE;
    uint32_t tmat = a; a = ksv_align128(a + tm);
    if (a > KSV_VRAM_HALF + h) return 0;          /* does not fit below the texture line */

    /* Half 0: nothing but the vertex buffer, up to the same texture line. */
    uint32_t vsize = h & ~127u;
    if (vsize <= b0->vertex_size) return 0;        /* no gain */

    pvr_ta_buffers_t nb = *(const pvr_ta_buffers_t *)b0;
    nb.vertex = 0;
    nb.vertex_size = vsize;
    nb.opb = opb;
    uint32_t acc = 0;
    for (int j = 0; j < PVR_OPB_COUNT; j++) {      /* same per-list split KOS uses */
        nb.opb_addresses[j] = opb + acc;
        acc += pvr_state.opb_size[j] * pvr_state.tw * pvr_state.th;
    }
    if (acc != opb_total) return 0;
    nb.tile_matrix = tmat;

    pvr_state.ta_buffers[0] = nb;
    pvr_state.ta_buffers[1] = nb;                  /* never targeted when single-buffered */
    pvr_state.frame_buffers[0].frame = f0;
    pvr_state.frame_buffers[1].frame = f1;
    memset((void *)(PVR_RAM_BASE + f0), 0, fb0 + fb1);   /* clear the moved framebuffers */

    pvr_init_tile_matrices(presort);               /* KOS: rebuild matrices for the moved OPBs */
    pvr_sync_view();                               /* KOS: vid_set_start(front framebuffer) */
    pvr_sync_reg_buffer();                         /* KOS: TA OPB + vertex buffer registers */
    return vsize;
}

#endif

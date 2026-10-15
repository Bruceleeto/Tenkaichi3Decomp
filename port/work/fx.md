# Task: the frame-buffer effects as records and render targets (worklist phase 6)

Read `port/work/common.md` first. Worktree `/home/bruce/Downloads/bt3-wt/fx`, branch `work/fx`.
Your branch has the DC float fix (18ea88a): your DC build expects `123 stubs`, not 126.
Three other agents are working on other branches: `rnd_gs.c`'s OT decoder (ot), `gs_pvr.c` pkt_key (pal),
VU1 calls (vu1). To keep merges easy: put your new port code in NEW files (e.g. `port/src/rnd_fx.c`,
`port/include/rnd_fx.h` or additions at the END of `include/port/rnd.h`), and keep edits to `rnd_gs.c`,
`gs_pvr.c`, `gs_draw.c` small and local (no reformatting, no moving code).

## Decision already made (by the user)
Every frame-buffer effect is converted: it draws through records and render targets, works on PC (both PC
renderers: `BT3_GPU_API=pvr` and the GL one), and is OFF on the Dreamcast for now (VRAM): one switch, the
existing `no_rtt()` in `port/src/gs/gs_pvr.c` (always on under `__DREAMCAST__`, `BT3_PVR_NO_RTT=1` on PC).
When the switch is on, a converted effect draws NOTHING and sends NO packet (the game goes on as if the
effect were not there; check the effect's own state still advances). Later the user may turn single
effects on for DC: so write them for the PVR's abilities, not the GL's.

## Today
Every effect below builds GS packets (A+D FRAME/TEX0/... + sprites/strips) that copy the frame into work
pages, read them back as textures, read the Z buffer as an 8-bit texture through a CLUT, use destination
alpha as a stencil, etc. See `port/ps2_gs_audit.txt` section 2 (each line) and section 4 (M1-M6: the GS memory
layout: colour 0 / 0x70, Z 0xE0 (TBP 0x1C00), work pages 0x150 / 0x16C / 0x170 / 0x1F6 / 0x1F8).
Records (`include/port/rnd.h`): `RndState.fbp` already chooses the target drawn into (`GsDraw_TargetGet`).
Sampling a target today only works through the GS fallback (`gsTex0` naming the page, `tex_is_target` in
`gs_draw.c` / `gs_pvr.c` `draw_state`). Read how RTT works in gs_pvr.c (run_flush, capture_update,
frame_render) and gs_draw.c before designing.

## Step 1: the API (one commit)
- A record names a render target as its texture: e.g. `uint32_t Rnd_TargetTex(uint32_t fbp, int w, int h,
  int psm)` returning a handle usable in RndQuad/RndPoly `tex` (with `pal` 0), resolved by both renderers to
  the target's current contents (as tex_is_target does). Coordinates as for any texture (0..1 / sixteenths).
- `int Rnd_FxOn(void)`: 0 when the switch says no targets (DC). Effects test it once at their top.
- A clear of a target region (for GfxPost_ClearWork, ObjShadow_AddTargetEnv's clear sprites) as a record
  (Rnd_Rect / Rnd_Fill into that fbp is likely enough).
Ask if the existing target machinery cannot name a target by fbp without GS memory.

## Step 2: the straight conversions, one commit each
Frame copies, downsample chains, displaced/stretched redraws: same pages, same sizes, same order, records
instead of packets (keep each original under `#ifdef PORT ... else`, as other converted sites do).
1. GfxPost_ClearWork, PadWatch_DrawMessage (fill + font) - small, to prove the API.
2. ObjShadow_AddTargetEnv (render into the shadow page 0x150; the shadow draws themselves are VU1 = later,
   leave them; only the target set-up and clears).
3. StgPanBlur_CopyScreen / StgPanBlur_DrawView / StgPanBlur_Draw (keep the Port_FxOff(32) gate).
4. GfxPost_DrawGlow (downsample chain 0x150 -> 0x170 -> 0x1F6/0x1F8, blur, add back).
5. StgBlur_BeginDraw / Draw / EndDraw (radial blur ping-pong 0x150 / 0x16C).
6. ScrWarp_BeginPacket / Draw / EndPacket (+ BeginStrip / PutVertex).
7. GfxLens_PutCapture / DrawOne / Draw.
8. GfxWater_DrawView (non-matching C: edit inside its ATTEMPT block).
9. ScrXfade_StoreHalf / Draw: no readback to RAM; keep the half as a target and draw it back.

## Step 3: the depth readers (M2) and alpha-stencil users (M3), one commit each
StgFog_Draw, StgHaze_*, StgGlare_Draw, StgDepthTint_Draw, GfxDepthFog_Draw (+ GfxPost_ShiftHighWord,
GfxPost_CopyAlphaToDepth), GfxAlphaKey_Draw, ObjGlow_Draw, ObjOutline_Draw, GfxPost_DrawDepthClut(At),
GfxPost_DrawTex16Texa.
The PVR cannot read depth as a texture or use a CLUT on a frame buffer. For EACH: first read what the
effect computes (which depth range -> which alpha/colour, what it is blended with), then write a short
design (5-10 lines) into `port/work/fx_designs.md` in your worktree: what it looks like, what the PS2 does,
the PVR-shaped substitute (e.g. depth-based fog via vertex/fog-table on the drawn geometry, the tint applied
to the geometry's colours, a stencil via the PVR modifier volumes / alpha-only pass, an approximation that
ignores depth), and what will differ. Implement it if it stays within the port (no new renderer feature
bigger than a small function); if it needs a renderer feature, stop and ask with the design written.
Those that already have a native GPU path (`Port_GsMarker(PORT_FX_ALPHA_KEY)`, the outline marker,
`Port_FxOff`) - keep that behaviour and say how it maps.

## Not yours
ObjDraw_AddClutPass (Port_ClutTint covers it), the dead GfxPost_DrawTex16 / GfxPost_DrawTexRect, the VU1
shadow draws, anything in the OT.

## Checks (no game runs)
Both builds, every commit; implicit-declaration check. In each commit message: the effect, the pages /
targets it uses, and what happens with the switch on (DC). You cannot see the result: the lead tests each.
Make that easy: one effect per commit, and list in your report which scene/stage/move shows each effect if
you can tell from the code (stage ids, skill ids, menus).

## Stop when
Steps 1-3 done, or blocked (ask). Report as common.md says, plus a table: effect -> commit -> targets ->
substitute (for step 3) -> how to see it in game.

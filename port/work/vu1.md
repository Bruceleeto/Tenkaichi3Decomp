# Task: VU1 programs as C that outputs records (worklist 5.1, 5.2, 5.3)

Read `port/work/common.md` first. Worktree `/home/bruce/Downloads/bt3-wt/vu1`, branch `work/vu1`.
Spec: `port/ps2_offline_audit.txt` section 1 (per program) and "REPLACEMENT SPEC"; section 2 (the three chain
families, header sizes 5 / 1+3 / 3 qw, V4-32 only). Microcode: `src/vu1/prog*.vsm` with `docs/systems/vu1/*.md`.

## Today
`Vu1Pkt_CallProgN` (`src/sys/vu1_packet.c`) put a VIF chain (constants + a call of the asset chain) into the
DMA list. At flush, `port/src/gs/gs_vu1.c` executes it: either the microcode interpreter or the HLE paths
`hle_program0/1/2/4/6/78`, which already turn batches into triangles (GsGpu_DrawVu0/Vu4/Vu6) but only inside
the GS emulation, at flush time, from VU memory.

## Goal
At the CALL site, under `#ifdef PORT` and `Port_TakesRecords()`: walk the asset chain's batches directly in RAM
(the chain address the caller passes; `Port_Ptr` for u32 game pointers), run per-program C, and queue
`RndPoly` records (`include/port/rnd.h`, `Rnd_Draw` / `Port_Poly`) at the display-list cursor, exactly where the
call packet would have been (records already order themselves that way). Otherwise the call packet as now.
The constants the program would get (matrices, light, screen, clip, colour) are what the caller is about to
put in the packet: take them from the same source, not from VU memory.

## Code layout
- New `port/src/vu1_native.c` (+ header in port/include): the batch walker (one per family) and one function
  per program. Pure functions: (constants, batch) -> records. No GS memory, no gs_core, no gs_vu1.
- Math in small static helpers (mat4 x vec4 column-major as the spec says, dot, lerp, project), so they can
  move to sh4zam later. No asm.
- State: the record's RndState comes from `gRndCtx[ctx]` via `Rnd_Draw` (the env packets are followed),
  ctx from the strip tag (layer 0 ctx 1 = GS context index 0; layer 1 ctx 2 = index 1). Texture: the batch's
  TEX0 as `gsTex0` (the GS fallback finds it, as other records do today); handles are a later step.
- Strips -> triangles or 4-corner strip polys in the same winding the strip has; ADC corners kick nothing.

## Order, one commit each
1. prog4 (stage, `Vu1Pkt_CallProg4`, callers in src/battle/stgm_a.c): strips 0x5C; the 6-plane clipper's
   output as ordinary clipped triangles (clip in homogeneous space before the divide, same planes as the
   microcode); repeat UVs; colours as given (may exceed 0x80).
2. prog0 (fighter, `Vu1Pkt_CallProg0`, ObjDraw_DrawParts mode 0): 2-matrix skin, layer 0 textured ctx 1,
   layer 1 ramp ctx 2 with u = 0.5 + 0.5*dot(-L, n_skinned), ADC rule, no clipping.
3. prog1 (fade pass): single matrix, only batches whose first normal.w low 16 bits are nonzero, layer 1
   texture from constant 26.
Stop after these three (shadows 2a/2b/6, debris 7 and nodes 8 come later, after the lead tests these).

## Checks (no game runs)
- Both builds every commit; implicit-declaration check.
- A standalone harness under `port/tests/work/` that takes real batches from the game files (use
  `gamedata/`: a stage file for prog4, a fighter model file for prog0/1; the offline audit's scanner shows how
  to find them) and compares your output to the reference for the same constants: preferably the HLE path's
  triangles from gs_vu1.c (factor or compile it in with stubs), or the microcode interpreter. Same triangles,
  corners within 1/16 pixel and 1 z step, same colours, same s/t/q within 1e-5 relative. Report the counts
  compared and the worst error. For prog4, include batches that need clipping.
- Note what the harness cannot check (state, textures) in the report.

## Stop when
prog4, prog0, prog1 are done and checked, or something is unclear (ask). Report as common.md says.

# Task: the ordering table's draws as records at the source (worklist phase 4.1)

Read `port/work/common.md` first. Worktree `/home/bruce/Downloads/bt3-wt/ot`, branch `work/ot`.

## Today
The effect code writes GIF packets into the ordering table (OT): `gOtCur` hands out memory, the packet is an
`OtPrim` header (`include/sys/gfx_ot.h`) plus GIF data, linked by z/layer (`Ot_Add`, `EftOt_Add`,
`EftTOt_Add`, ...). At draw time `Ot_Send` (`src/sys/gfx_ot.c:180`) walks the list, and `Rnd_OtTake` /
`Rnd_OtDraw` (`port/src/rnd_gs.c`) DECODE each packet back into `RndPoly` records (`vertex()` there shows exactly
how each PRIM type becomes a polygon: triangle, strip, fan, sprite, line). Packets it cannot take are sent, and
their register writes are followed into `gRndCtx` (`Rnd_OtState`).
Goal: producers say what they draw directly, so the decoder can go later (4.2, not yours).

## Step 1: the record node (one commit)
Add an OT node that carries a draw instead of GIF data, under `#ifdef PORT`:
- An `OtPrim` with `qwc` 0 and a distinct `tagId` (pick one no DMA tag id uses; document it), followed by
  `OtRec`: `u8 ctx, abe, gouraud, tme, fst, type, n` (type: 3 triangle, 4 strip of two (0 1 2, 1 2 3),
  6 sprite (two corners), 1 line) and up to 4 corners IN GS FORM, exactly what the packet would hold:
  x, y in sixteenths including XYOFFSET, 24-bit z, s t q floats (or u v sixteenths when fst), rgba (r low byte).
  Size a multiple of 16 bytes. Allocated from `gOtCur` like the packets (same alignment rules).
- `Ot_Send`: for a record node, close the direct block as the taken-packet path does, then
  `Rnd_OtRec(rec)` (new, `rnd_gs.c`): builds the `RndPoly` under the state followed AT THAT POINT (gRndCtx,
  sXyOff, sTex0 of the context: the same values `vertex()` uses), the same corner handling as `vertex()`
  (sprite expansion and s/t division, line as one-pixel quad, fst u/v -> s/t by TEX0's size), then `Port_Poly`.
  Share the code with `vertex()` (factor a function both call) rather than copying it.
- If the state cannot be drawn (`gRndCtxBad[ctx]`), `Rnd_OtRec` writes the equivalent GIF packet at the
  direct block's cursor instead (PRIM by A+D, then RGBAQ / ST or UV / XYZ2 per corner) so nothing is lost.
  Same for `!Port_TakesRecords()` (cannot happen if producers check, but keep it safe).
- A helper for producers, e.g. `OtRec *Ot_NewRec(void)` (from gOtCur) + they fill it + link it with the same
  Ot_Add / EftOt_Add call they use for the packet. Count recs in `gRndOtStat` (a third counter).

## Step 2: convert the producers, one commit each (or one per shared helper)
List: `port/ps2_gs_audit.txt`, every line marked `-- OT -- DECODED` (39) and `DECODED+STATE` (4), plus
`STATE+DECODED` (1). Skip the DEAD ones (EftPrim_DrawBillboard, EftUtil_DrawTri) and the Battle_Draw /
ChrView_Update / ShenScene_Update lines (they are the drain, not producers).
- First look for shared packet builders (E4 / C4 strip macros, EftOt_Add / EftTOt_Add callers, the
  EFT_SURF_FILL_PKT / EFT_STORM_SPRITE helpers): convert the builder once where all its users write the same
  shape, and say in the commit which producers it covers.
- Each converted site: `#ifdef PORT if (Port_TakesRecords()) { record node } else #endif { the original }`.
  Same values the packet carried: same PRIM bits (ctx = PRIM bit 9 / CTXT, abe bit 6, gouraud bit 3,
  tme bit 4, fst bit 8), same corners in the same order, same z, same colours.
- A packet that ALSO sets state (CLAMP, TEST by A+D before its draw: EftMesh_QueueTri, EftGeyser_DrawColumn,
  EftWater_*): keep that state part as a small packet node (it is followed as now), then the record node,
  linked in the same order into the same entry.
- A producer whose packet holds several draws (rings, fans, columns): several record nodes, or a record
  node with a count, your choice; keep the order.
- Non-matching C (`#if 0 /* ATTEMPT` blocks, the port compiles them as `#if 1` into port/build/gen): edit the
  C inside the ATTEMPT block, which is what the port builds.

## Checks (no game runs)
- Both builds, every commit. Implicit-declaration check on every touched file.
- A standalone test under `port/tests/work/` that feeds the same packet to the old decode path and the
  equivalent OtRec to `Rnd_OtRec` and compares the RndPoly produced (stub Port_Poly to capture). Cover each
  PRIM type the producers use: tri 0xB/0x1B/0x5B, strip 0x14/0x1C/0x54/0x5C/0x25C, sprite 0x46, line 1/2.
- In each commit message name the producers converted.

## Stop when
All producers on the list are converted, or you hit something unclear (ask). Report as common.md says,
plus a table: producer -> commit -> PRIM types -> any state packet kept.

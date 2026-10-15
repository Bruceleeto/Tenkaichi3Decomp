# Task: fighter palettes found in the texture pack (worklist 0.3, 0.4, 0.5, 1.1)

Read `port/work/common.md` first. Worktree `/home/bruce/Downloads/bt3-wt/pal`, branch `work/pal`.
Game data: `gamedata/` in your worktree (symlink, read only). The offline audit's scripts are in
`/tmp/claude-1000/-home-bruce-Downloads-Tenkaichi3Decomp/2ee54c34-0b10-4586-a502-fba720a3439b/scratchpad/offline/`
(read only: copy what you need into `port/tests/work/offline/` in your worktree and commit them there).

## The problem (port/ps2_offline_audit.txt section 3)
`BtlObjMdl_InitAlphaSlots` (`src/sys/gfxm_e_b.c:124`) rewrites the alpha of the first 16 CLUT colours of
fighter material palettes to `slot + alphaRow * 15` before the upload. `ObjDraw_UploadTextures` ->
`Port_PalNative` (`port/src/gs/gs_pvr.c`) then hashes the REWRITTEN palette (`pkt_key`), while
`port/tools/dc_tex.c` hashed the file's palette when it built `tex.pak`. Result: 8427 of 9108 fighter
palettes miss the pack (681 hit for one row only) and fall back to GS memory.

## Step 1 (no code change): 0.3, what the slot alphas are for
Read `BtlObjMdl_InitAlphaSlots`, `BtlObj_UploadAlphaTable` and every pass that draws a fighter with those
palettes (ObjDraw_*, ObjGs_*, prog0/prog1 envs, the CLUT-pass / dim pass ObjDraw_AddClutPass, Port_ClutTint).
Answer: which GS state reads palette alpha (ALPHA blend, TEST alpha ref, TEXA, FBA, the alpha table's own
draws), and therefore whether the native palette MUST carry the rewritten alphas (to draw right) or only
needs to be FOUND. Write the answer into your report and into the commit message of step 2.

## Step 2: 1.1, the key (one commit, or two: key, then pack)
Make the runtime key and the pack key agree for these palettes. Preferred, if step 1 allows it:
- The key ignores the 16 slot alphas: in `pkt_key` (gs_pvr.c) and the matching code in `dc_tex.c`, for the
  palette kind InitAlphaSlots touches (decide how to recognise it without guessing: e.g. a flag passed by
  `Port_PalNative`'s caller, not a heuristic on the data). The native palette then gets the live alphas
  copied in after the lookup (the 16 bytes), so what is drawn matches what the GS has.
If step 1 says something else is needed, stop and ask before coding.
- Rebuild the pack IN YOUR WORKTREE (`make -f Makefile.dc dc-assets`, output under your worktree's
  `port/build/dc/assets/`). Never write the main tree's tex.pak. Report its size before and after.

## Step 3: the check (static)
Extend the copied `tex_check.py` (or a C harness linking the real `pkt_key`) to simulate what the runtime
hashes: every fighter material palette rewritten for every alphaRow 0..14 the code can assign (read how
alphaRow is chosen and say which rows are possible), keyed with the new key, looked up in YOUR new pack.
Target: 0 misses. Also re-run the original full coverage (89786 entries): still 0 misses. Commit the scripts.

## Step 4: 0.4 and 0.5 (no code change unless trivial)
- 0.4: who consumes the 33 prog0-layout chains in "BObjMdl.unk84" parts and the 5 in the character effect
  pack (read the binders: obj->mdl.extra[], MdlTex_RebaseChain callers). Are their textures/palettes covered?
- 0.5: global maximum of stage vertex colours (scanner pass over all stage batches: prog4 and prog8 data),
  per channel, and how many batches exceed 0x80.
Put both answers in the report.

## Stop when
Steps 1-4 done, or step 1 needs a decision (ask). Report as common.md says.

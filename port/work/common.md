# Rules for every work agent

You are one of three agents working in parallel on the native port of DBZ Budokai Tenkaichi 3 (PS2 decomp).
The goal: the game calls the renderer directly (records, `include/port/rnd.h`) instead of building PS2 GS
packets, so the Dreamcast build can drop the GS emulation (`port/src/gs/gs_core.c` packet paths,
`port/src/gs/gs_vu1.c`, 4 MB of GS memory). Background: `port/ps2_worklist.txt` (the plan),
`port/ps2_gs_audit.txt` (every producer, R/D checklist), `port/ps2_offline_audit.txt` (VU1 specs, asset
formats, texture pack coverage). All three are in the MAIN tree: `/home/bruce/Downloads/Tenkaichi3Decomp/port/`.

## Where you work
- Your own git worktree and branch (given in your task file). Edit, build and commit ONLY there.
- Never touch the main tree `/home/bruce/Downloads/Tenkaichi3Decomp` except to READ (audits, game data under
  `gamedata/`, `port/build/dc/assets/tex.pak`). Never touch the other agents' worktrees.
- `asm/`, `assets/` and `.venv/` in your worktree are symlinks into the main tree: read only.
- Never touch `/home/bruce/Downloads/iortcw` or any other repo. No sudo (if something needs it, stop and say so).

## Building (both, before every commit)
- PC: `make -j16` in your worktree. Expect `103 stubs; link OK -> port/build/bt3; 0 error lines`.
- DC: `source /opt/toolchains/dc/kos/environ.sh && make -f Makefile.dc -j16`. Expect `126 stubs; link OK`.
- A stub count that changes means a function lost or gained a definition: explain it or fix it.
- Port sources are globbed (`port/src/*.c`); after renaming a file delete its stale objects under
  `port/build/obj` and `port/build/dc/obj`.
- Missing prototypes: `gcc -m32 -std=gnu89 -fsyntax-only -DPORT -Iinclude -Iport/include -include port_compat.h
  -include port_libm.h -I<dir of the file> -Wimplicit-function-declaration <file>`.

## Running
- Do NOT run the game (`port/build/bt3`, any window). Nobody runs it but the lead.
- You MAY build and run small standalone programs or python scripts (python with `-I`) that read game data
  or test a function in isolation, written under your worktree's `port/tests/work/` or the scratchpad you are
  given. Never `addr2line` without addresses (it reads stdin and hangs).

## Code
- Game code under `src/` stays decomp-faithful: port changes go inside `#ifdef PORT`, keep the original path
  when records are not taken (`Port_TakesRecords()` 0: software renderer, `BT3_RND=0`).
- Match the surrounding style: comment density, naming, the file's idiom. Comments say what the code does,
  in plain sentences, like the existing ones.
- Vector/matrix math in small helpers (it will later move to sh4zam). No hand-written SH-4 asm (no ftrv).
- No new dependencies.

## Commits
- One commit per converted item (so it can be bisected), on your branch, after both builds pass.
- Commit message: what changed, in the repo's style (see `git log --oneline -20`). NO Co-Authored-By or any
  Claude/AI attribution line. Do not push. Do not merge or rebase other branches.

## When you are unsure
- Do not guess on anything that changes what is drawn or how data is read. Stop at a clean point (everything
  committed or reverted, both builds passing) and END with your question as your final report. The lead
  answers and resumes you.

## When you are done
- Stop when your task file's list is done (or you are blocked). Final report, terse:
  commits (hash + one line each), what each was checked against, anything left undone and why, open
  questions, anything you noticed that is wrong elsewhere (do not fix things outside your task).

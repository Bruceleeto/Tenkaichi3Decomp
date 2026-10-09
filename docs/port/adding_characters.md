# Characters from outside the disc (work in progress, branch `charslots`)

Started 2026-10-08. The aim: a `characters` folder next to the game, one folder per character holding a
community character pack as it comes, each taking a slot of its own instead of replacing a disc character.

## State

- Battle side: works. With `BT3_TEST_CHARA=165` the recorded two-player session plays to its end with the pack's
  character on side 0: its model, animations, effects and voice bank are read from the folder, the rest from the
  base character; seen on screen (the model, its moves, hits).
- The duel's character select: an added character has a cell after the disc's (in the last row's filler cells,
  then rows of its own), with its base character's chip, portrait and name for now, the folder's number of
  costumes, and no item sets. Chosen there with a recorded session (`BT3_TEST_CELL=1` starts the cursor on it),
  the fight is the same as with `BT3_TEST_CHARA`; seen on screen. With a character installed and not chosen,
  the recorded session ends as it always does.

## The pack format (seen in one pack, "Gokhan", 2026-10-08)

Eight files in the game's own format, the same layout as the disc's files of a character:

| file | the disc's file |
|---|---|
| `*_1p.pak` .. `*_4p.pak` | model of costume 1..4: `0x590 + id * 10 + 0..3` |
| `*_1p_dmg.pak` .. | damaged model: `+ 4..7` |
| `*_anm.pak`, `*_eff.pak` | `+ 8`, `+ 9` |
| `*_Voice_US.pak`, `*_Voice_JP.pak` | voice bank, `0xBDA + id` and `0xC7B + id` |

Inferred, not checked: which of the two bank ranges is which language (`_Voice_US` is taken as 0xBDA, the one
used when `SAVE_FLAG_VOICE` is clear); that the `_0_` in the names is a form number. The pack has no portrait,
no icon, no name, no parameter file and no streamed voice lines.

## What hangs on a character id (survey of the sources, 2026-10-08; verified by reading unless marked)

- The disc has 161 characters, 0x00..0xA0. 0xA1 random, 0xA2 locked, 0xA3 custom, 0xA4 empty are cells of the
  character select, in the disc's list (pack section 42) as well as in the code, and are used as indices too
  (the chip pack has 165 lists, the portrait files and the name lines are taken with the cell's id). The first
  id without a meaning is 0xA5.
- Limits on an id: below 0x100 (from there the battle loads an object), not 0xFF ("none" in the byte lists of
  transformations, fusions and links), at most 191 for a bit in `charaBits` (3 x 64). So 0xA5..0xBF, 27 ids.
- File ids, each rule packed for 161 characters (the id after character 160's is the next kind's first):

| rule | files | character 161 would get |
|---|---|---|
| `8 + id * 2 + side` | parameter block (0x1000 bytes) | the two common sound banks |
| `0x2F9 + id` | portrait (menus) | an unused file; from 164 the stage pictures |
| `0x590 + id * 10 + n` | models, animations, effects | the voice banks of characters 0..9 |
| `0xBDA + id`, `0xC7B + id` | voice bank, two languages | the other language's first; the first battle object |
| `0x8D4E + id * 100 + line`, `0xCC32 + ...` | streamed voice lines | the other set; the music |

  The second archive ends at index 65200; ids from 0x10BF9 are free. `File_GetSize` waits for ever for a file
  that cannot be opened: an id must always be answered.
- `obj->chara` is taken back from the model's file id (`(id - 0x590) / 10`, btl_obj.c `BtlObj_Setup`).
- Tables with a row per character and no bound check: `paramTbl` (health, ki, blast stocks; common file 2),
  `ChrTblEntry` (AI type, costumes, cost, level, links, the viewer's camera; common file 4), and in every
  character's own model file `talkTbl[opponent * 4]` (what is said to whom).
- The save: `charaBits` has room; `custom[97]` is indexed by the CELL of the character select (column + row * 7),
  and is written by the customising screens: a cell from index 97 on writes into `rec[]`. An added cell must not
  have item sets.
- Other screens index their packs with a character id taken from a save or a replay (rankings, replay headers,
  passwords: 8 bits, checked `> 0xA0`).

## How it is done (include/port_chara.h, port/src/plat_charas.c)

- An added character is 0xA5 + n, the folders taken by name.
- Its ten model-rule files are ids `0xD48 + 71000 + n * 10 + k` and its two voice banks `0xD48 + 72000 + n * 2 +
  language`, past the archive's end; `PortCharas_Alias` answers them with the folder's file, or with the base
  character's disc file where the folder has none (a missing damaged model: the costume itself; one language
  only: that one for both). `CHARA_FILE10`, `CHARA_VOICE_FILE`, `CHARA_FROM_FILE` put this into the game's rules.
- For every table and for the streamed lines it stands as its base character (`CHARA_T`): `base.txt` in the
  folder holds the id, 0 without it.
- `BT3_TEST_CHARA=<id>[,<id>]`: the first fighter of side 0 (and 1) of any battle the menus set up.

## Where a character's pictures and name come from (traced in the sources, 2026-10-09)

Four things, in four places. Verified by reading the code unless marked.

| what | where | used by |
|---|---|---|
| the face on the battle HUD | entry 1 of the character's own model file (`*_1p.pak` ..), relocated by `Res_RelocateOffsets`; the fighter object keeps it at +0x50 and `HudGauge_DrawFace` (hud_b.c) and the team HUD (hud_a_b.c) take it from there every frame | the fight |
| the large picture | a file of its own, `0x2F9 + id` (`CHARSEL_FACE_FILE`), read into a 0x16800-byte buffer | character select (menu_c_e.c), tournament bracket (menu_k_f.c), entry select (menu_i_d.c), ranking (menu_n_c.c), character reference (menu_h_b.c) |
| the chip (the small square of the roster) | a pack of texture lists inside the screen's resource pack, entry `id + 1`: section 45 of the character select's pack, section 6 of the ranking screens' | every screen with a roster; each screen's pack has its own copy |
| the name | a text block inside the screen's resource pack, one line per id: section 43 of the character select's pack (`nameText`, drawn with `TextBox_Init(box, nameText, id + 1)`); sections 30, 28, 21, 22 in other screens | every screen that shows a name; each has its own copy |

The screens' resource packs come from the menu archives: `gMenuArc5` is file `baseFile` and `gMenuArc6` file
`baseFile + 13` (menu_g_b.c, menu_h_c.c); the character select takes its pack from one of them by section.

What this means for an added character:

- **The HUD face needs nothing more**: it comes with the model file. A pack whose model file has its own entry 1
  shows its own face. (Seen 2026-10-09 with a converted character from a mod: the face on the HUD was the pack's.)
- **The large picture** can be a file in the folder, answered for `0x2F9 + id` as the model files are
  (`PortCharas_Alias`); today `CHARA_T` asks for the base character's.
- **The chip and the name are not files**: they are entries of packs that each screen unpacks from the menu
  archive, so a folder's own chip and name have to be put into the screen's tables after it has unpacked them
  (as the names of added stages are drawn by the port), or drawn by the port over the cell. Today `CHARA_T` gives
  the base character's.

Not looked at: the name shown in a fight (the versus screen before it, the pause menu, the result), and the format
of a chip's texture list and of a name line beyond what the code above shows.

## Not done

The character's own name, chip and portrait on the character select; the cursor reaching the cell by hand
(only the test option was used); the team select and the other screens; the HUD's face (the test run showed the recorded
choice's); transformations into or from an added character; hiding them in an online session; zip files; which
base a pack expects.

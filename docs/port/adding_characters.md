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

- An added character is 0xC6 + n (0xA5 + n until 2026-10-09: see "57 characters"), the folders taken by name.
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

Located in the data (2026-10-09, read out of the disc's files with the game's own decompressor):

- The menu archive `gMenuArc5` is file id 0x1C1 (`pzs3us1/00448.bin`): a pack of two screens, each BPE-packed
  (`Bpe_Decode`: size, packed size, blocks). The second, 7,074,880 bytes unpacked, is the character select's pack
  of 56 sections.
- **Names**: its section 43 is itself a pack of 168 entries, one per id (0..160 the characters, then "Random",
  "? ? ?", "Password Character", "Not selectable" and three more). An entry is 64 bytes: the UTF-16 byte order
  mark FF FE, the name in UTF-16, zeros. Section 44, the same shape, is the form names.
- **Chips**: its section 45 is a pack of 165 entries of 5,504 bytes each, one texture list per id (a header of
  offsets, then the picture), all the same size.

So a folder's `name.txt` becomes a 64-byte entry and a folder's chip is one 5,504-byte entry in the disc's own
format: both can be put into the unpacked pack's tables (which then need room for more entries than the disc's
168 and 165) or kept beside them and handed out for the added ids.

Not looked at: the name shown in a fight (the versus screen before it, the pause menu, the result); the other
screens' copies (same shapes expected, not checked); the layout inside a chip's 5,504 bytes.

## The other screens with a roster (2026-10-09)

The added cells are now appended in `ChrGrid_Build` (view_a_e.c), which every screen with a grid calls, so they are
in all of them at once: the duel and team select, the one-fighter select of the Ultimate Battle modes and Sim
Dragon (`SoloSel`), the Ultimate Battle team select, the tournament's entry select. Left out on purpose: the
customising screens (modes 0x30..0x32), which write the save by a cell's place, and an online session.

What each of those screens does with a character id goes through `include/port_chara.h`: `PORT_CHIP` (its own
chip; `PortChips_Load` / `PortChips_Free` keep the folders' chips in the game's memory while a screen is open),
`PORT_NAME_LINE` with `Port_CharaNamePack` (its own name), `CHARA_FACE_FILE` (its own large picture),
`PORT_IS_CHARA` (the disc's `id < 0xA1` took an added character for a marker: no DP cost, never dimmed), and one
item set only (the sets are kept in the save by the cell's place, which an added cell does not have). The
tournament bracket shows the chosen character's chip, name and pictures the same way; the ranking screen of
Ultimate Battle and Sim Dragon's day screen its picture (the ranking's name line is the base character's: that
screen's text pack holds more than names).

The large pictures' file numbers moved from 73000 to 80000: 73000 is where added songs are, and with songs
installed the first characters' pictures were answered with song files.

Checked: builds; the replay check with and without characters; two recorded sessions with characters installed
(the grid is built with the added cell, no crash); the duel select as before. NOT seen: any of the newly covered
screens (no recorded session reaches them).

Not covered: the character reference (a list of 161 entries with its own tables, not a grid), the customising
screens, the replay and password screens.

## 57 characters (2026-10-09)

The limit was 27, ids 0xA5..0xBF. It is 57 now, ids **0xC6..0xFE** (`PORT_CHARA_FIRST`, `PORT_CHARA_MAX` in
include/port_chara.h and port/include/plat_charas.h). The range starts at 0xC6 and not at 0xA5 because the battle
takes a prop's "character" from its model file's number as it does a fighter's, which for the props' files comes
out as 0xC1..0xC5: a wider range from 0xA5 would have taken props for added characters. `BT3_TEST_CHARA` therefore
takes 198 for the first added character where it took 165. The folder scan reads 256 names (it stopped at 64),
and the large pictures' file numbers moved to 90000 (the spoken lines of 57 characters run from 74000 to 85400).

Checked with 54 folders installed (converted characters, four of them forms of others): all 54 are found, the
grid gets 50 cells for them (cells 99..148 of 165), the recorded session runs, a fight as the first and as the last
of them runs, the row of the select screen shows their chips; the replay check is unchanged with none and with 54.
Not checked: the other screens with that many, the menu's memory with 57 chips loaded (about 310 KB of it) on
screens that use more of it than the duel select.

## Random picks (2026-10-09)

The "random" cell of the select screens and the tournament's computer entrants are drawn from the grid with the
disc's test `id < 0xA1` (`<= 0xA0`), which passes over added characters. The setting "Added characters in random
picks" (F1, Mods; `chara_pool`, off unless set; `BT3_CHARA_POOL=1`) puts them in the draw: `PORT_IN_POOL` in the
duel and team select's random cell, the tournament entry select's two draws and the bracket's computer entrants.
A cell drawn that way lies past the save's item sets (`custom[97]`, by the cell's place): the reads that follow a
draw are guarded with `PORT_SAVE_CELL` (found while doing this: with the draw opened, they would have read past
the array).

Seen without a window: the computer playing an added character through a recorded fight (it moves, attacks and
is hit; its behaviour is its base character's). Seen by the user on 2026-10-10: a random draw landing on an added character, and added characters among a tournament's entrants.

## Fusion partners (2026-10-10)

A fusion needs a partner on the team, and the character's own table (entry 18, +0xBA, four ids for each of the three
fusions) names it by the numbers of whoever made the pack. For an added partner those numbers mean someone else
here, so the fusion could never be started. `forms.txt` takes `partner1=Folder,Folder,...` (also `partner2`,
`partner3`; up to four, `disc:<id>` for a character of the disc), resolved like the form lines
(`Port_CharaFormTarget` slots 7..18, used by `BtlParam_GetUnkBA`). Without the line the table's own ids are used.

The input (src/battle/btl_act_j.c): R3 HELD for 12 frames with left / up / right = fusion 1 / 2 / 3; a tap is the
transformation. Team battle only, the partner present and alive, the blast stock the fusion costs.

Seen by the user (2026-10-10): a fusion of two added characters into a third. Two faults seen with it, both
changed since and NOT seen again yet:

- **The partner was not drawn in the fusion's scene.** The scene shows the character at +0xB7 of the table, again
  its maker's number. For an added fighter the partner found on the team is shown (`BtlAct_QueueFusion`).
- **Team mates had their base characters' small pictures in the fight.** That picture is the fight's member block
  (file 8 + id * 2 + side, 0x1000 bytes, one for each side), which was asked for by the base character. A folder's
  `member_1p.bin` / `member_2p.bin` are served now (`CHARA_MEMBER_FILE`, file index 91000 + n * 2 + side); without
  them the base character's.

## Larger models, and a folder made from a data folder (2026-10-10)

- **The model buffers follow what there is to load.** They were 1 MB with any added character. A disc whose 161
  characters had all been replaced (same layout, standard archives, 804 bytes of its program changed) has 50 model
  files over the original's 0xCE000 and 16 over 1 MB, the largest 1,150,125 bytes. `Port_ModelSize`
  (port/src/plat_charas.c) takes the largest model among the added characters and the data folder's own
  (0x590 + id * 10 + 0..7), in whole pages, up to 1.5 MB (`PORT_MODEL_MAX`); an added character with a larger file
  is left out. Online: the original's size, as before.
- **Such a disc's data runs as a data folder** (`BT3_DATA`) with the original program file beside it (the port's
  tables come from that file, and its check passes): menus and a fight, windowless. Not seen on a screen. What the
  disc's own changes to the program do was not looked at.
- **`port/tools/import_chara.py <data folder> <id> <folder>`** copies everything of one character out of a data
  folder of the game's layout into a character folder: models, animations, effects, voice banks, the spoken lines,
  portrait, member blocks, chip, name, `base.txt`. From the player's own game it makes a copy of a character to
  start a pack from. It says which other ids the character's table names (forms, fusions).

- **`--with-forms`** imports the transformations too, and theirs, a folder each (`<prefix><id>`) with a `forms.txt`
  naming the others; `--with-fusions` follows the fusions, their partners and the forms of those (refused past 57).

Checked: three characters of that disc imported, read by the port, and a recorded fight with the 1.15 MB one run
to its end windowless; the replay check. Seen by the user (2026-10-10): a set of two made with `--with-forms` from
that disc, chosen and transformed in a fight. NOT seen: the disc's data run whole on a screen; `--with-fusions`.

## Forms in folders inside the character's (2026-10-10)

A folder inside a character's folder (other than `voice_us`, `voice_jp`) that holds a character is a form of it: a
character of its own, placed right after its owner. One level. What its `base.txt` and `name.txt` do not say is the
owner's. A name in `forms.txt` is looked for, in this order: `..` (the owner, from inside a form's folder), a form
inside the same character's folder, a character of the top, `Character/Form`. Folders side by side work as before;
a fusion's result, which has two parents, stays at the top.

`import_chara.py --with-forms` writes this layout, named from the disc: the character's name (section 43 of the
select's pack) for the folder, the form's label (section 44, "Super Saiyan") for the folders inside, the prefix and
the id where the disc gives none.

The script also takes a disc image (`.iso`) in place of the data folder, reading its two archives in place, and
`--all` makes a folder for every character of the disc (seen: 161 characters in 141 folders, 2.2 GB): more than
the port takes at once, a shelf to copy from.

An added stage's two pictures in the select (2026-10-10), each a file beside its `.unk`, each optional: `<name>.thumb`,
the large picture behind the reel (the disc's file 0x39D + stage; served at file index 70100 + n), and `<name>.icon`,
the small one in the reel (one 5504-byte entry of section 46 of the select's pack; copied into the game's memory
while the select is up, `PortStageIcons_*` in port/src/plat_stages.c). Without them the stage borrows a disc
stage's, as before. `--stages` writes both. Seen by the user: the large picture. NOT seen: the small one.

`--stages` also writes `stage names.png`: the disc's 36 stage-name pictures (section 6 of the select's pack, nine
512 x 256 sheets of four, decoded as namefont.c does) one under another in the stages' order, to read the names from.

`--stages` copies a disc's stages (file 0x171 + stage) as `<Name>.unk` files for the `stages` folder, named after
the original's places (a stage's name on a disc is a picture); `--against <original>` leaves out the unchanged
ones. Seen: 33 of a disc's 35, all within the stage buffer; two of them taken by the game as added stages. NOT seen: a fight on one (a windowless run with `BT3_TEST_STAGE` gave the same numbers for both, so it does not show the stage was used).

Checked: sets made from two data folders read by the port with every link found (none reported missing), folders
side by side still read, the replay check. NOT seen: a set of this layout in the menu or in a fight.

## Brought up to the release (2026-10-10)

The branch has the release's line merged in (0.1.19: the room codes and relay, the tournament crash fix, the clash
value, the added songs' and stages' fixes for team battles). Not merged the other way. With it the team select
serves an added stage's own pictures too (`.thumb`, `.icon`), as the duel's select does here.
Checked: builds, the replay check with and without folders installed, a recorded menu-to-fight session with added
characters and stages. NOT seen: the options menu after the merge, the team select's own pictures.

## The save and replays (audit of the sources, 2026-10-10)

Every writer of the save and of a replay was read for an added id or a cell past the disc's 97. Three faults, and
what was done:

- **Evolution Z could write outside `custom[97]`.** The selects share one remembered cursor (gProgress + 0x440). Left
  on an added cell, it lay past the end of Evolution Z's grid (which has no added cells); the restore tested only
  `ChrGrid_IsSelectable`, a cell past the count read as character 0, and confirming wrote "item sets" over `rec[]`,
  `item[]`, the money. Fixed at the root: every restore (menu_c_e, menu_e_b, menu_l_b, menu_m_b, menu_i_d, menu_u_g)
  drops a cursor outside its own grid (`PORT_CELL_IN`). The same stale cursor made the other selects read item sets
  from past the table; gone with it.
- **Sim Dragon's ranking kept an added id** and its top screen then indexed a 165-entry face pack with it. The ranking
  now keeps the base character's id (`PORT_SAVE_CHARA`, menu_r_c.c), and the top screen maps an id already stored
  (menu_r_d.c).
- **A replay keeps added ids** (the whole battle setup). Left so: a replay of a fight with an added character needs
  that character, in the same place in the list. The replay list's chip lookup read out of range with such an id;
  it shows the base character's chip now (menu_za_d.c). Played with the folder gone, the id answers as character 0:
  no fault, but not the recorded fight.

Found safe: `charaBits` (ids from disc tables only), `rec[]`, the tournament's prizes, the port's settings (a 0/1
flag). gProgress is never written to the card.

Checked: builds, the replay check with and without packs. NOT seen: any of the three paths, before or after. A
fifteen-minute comparison of the save with and without a pack chosen showed nothing, because the recorded session
never reaches a point where the game saves.

## More than that (researched 2026-10-09, nothing changed)

The three reasons given above for the limit do not bind an added character: every character id in the game's
state, the save, the replay and the menus is 32 bits wide; the only one-byte ids are in disc and pack DATA (the
transformation and fusion tables, the character table's links); the unlock bits are never asked about an added
character. What limits the number today is the port's own: `PORT_CHARA_MAX` (defined twice), a scan of the first
64 folders, the file-number ranges (ten model files per character from 71000 hold 100 before they reach the voice
banks at 72000), and the grid: every screen's buffer is 165 cells and the disc's grid is about 98, so about 60
more cells fit (forms share a cell).

- Up to 90 (ids to 0xFE): raise the constants and the scan, give the grids more cells or the added characters a
  page of their own. To look at first: the battle takes a prop's "character" from its model file's number, which
  comes out as 0xC1..0xC5, inside the wider range; and a pack's own tables naming partners by number.
- Hundreds: ids from 0x100 are taken by the battle's props (0x100..0x12B) in one path (`BtlLoad_StepObject`, the
  three throw getters). The least invasive way seen: wide numbers in the menus only, with a scrolling or paged
  section for added characters, and the few chosen for a battle (at most ten, and their forms) bound to the
  numbers 0xA5..0xFE when the battle is set up; a replay then has to carry which folders they were.

## Not done

The character reference; fusion between added characters (wired, never tried); a form added to a character of the
disc; the save audit (what a play with added characters leaves in the save and in replays); zip files; a guide
to the pack format for people who make packs.

#ifndef PORT_CHARA_H
#define PORT_CHARA_H

/* PC build: characters added from outside the disc (port/src/plat_charas.c).
 *
 * The disc has 161 characters, ids 0x00..0xA0, and the menus use 0xA1..0xA4 as markers (random, locked, custom,
 * empty), in the disc's data as well as in the code. An added character has an id from 0xA5: below 0x100 (from
 * there the battle takes an id for an object), not 0xFF ("none" in the byte-sized form lists), and with a bit in
 * the 192-bit unlock field.
 *
 * Two things hang on a character id:
 * - file ids. Every rule (`0x590 + id * 10 + n` for the models, `0xBDA + id` for the voice bank, ...) is packed
 *   for 161 characters: the id after the last character's is the first of the next kind of file. An added
 *   character's own files (models, animations, effects, voice banks) have ids past the end of the second
 *   archive, which the port answers from the character's folder.
 * - tables of 161 rows in the common files (health and ki, cost, costumes, AI type, links) and of 161 entries in
 *   every character's own file (what is said to which opponent), and the streamed voice lines. For these an
 *   added character stands as its BASE character, one of the disc's.
 */
#ifdef PORT
#define PORT_CHARA_FIRST 0xA5
#define PORT_CHARA_MAX 27                          /* 0xA5..0xBF: the unlock field has bits for ids up to 191 */
#define PORT_CHARA_FILE0 (0xD48 + 71000)           /* ten files per added character, as on the disc */
#define PORT_CHARA_VOICE0 (0xD48 + 72000)          /* two per added character: the voice bank, and the other language's */
#define PORT_CHARA_ADDED(c) ((s32)(c) >= PORT_CHARA_FIRST && (s32)(c) < PORT_CHARA_FIRST + PORT_CHARA_MAX)
extern int Port_CharaBase(int chara); /* an added character's base character; any other id as it is */
/* the character id for the tables and the streamed lines */
#define CHARA_T(c) (PORT_CHARA_ADDED(c) ? Port_CharaBase(c) : (s32)(c))
/* `id * 10` of the model rule: CHARA_FILE10(c) + 0x590 + n is file n of the character */
#define CHARA_FILE10(c) (PORT_CHARA_ADDED(c) ? PORT_CHARA_FILE0 - 0x590 + ((s32)(c) - PORT_CHARA_FIRST) * 10 : (s32)(c) * 10)
/* the character a model file belongs to (the battle takes an object's character from its file id) */
#define CHARA_FROM_FILE(id) \
    ((id) >= PORT_CHARA_FILE0 && (id) < PORT_CHARA_FILE0 + PORT_CHARA_MAX * 10 ? PORT_CHARA_FIRST + ((id) - PORT_CHARA_FILE0) / 10 : ((id) - 0x590) / 10)
/* the voice bank: `base` is one of the two languages' first file ids, `alt` says whether it is the second */
#define CHARA_VOICE_FILE(c, base, alt) (PORT_CHARA_ADDED(c) ? PORT_CHARA_VOICE0 + ((s32)(c) - PORT_CHARA_FIRST) * 2 + ((alt) ? 1 : 0) : (s32)(c) + (base))
/* the large picture of the menus: 0x2F9 + id on the disc; an added character's own file, answered from its folder
   (`portrait.bin`) or with its base character's */
#define PORT_CHARA_FACE0 (0xD48 + 73000)
#define CHARA_FACE_FILE(c) (PORT_CHARA_ADDED(c) ? PORT_CHARA_FACE0 + ((s32)(c) - PORT_CHARA_FIRST) : (s32)(c) + 0x2F9)
#else
#define CHARA_FACE_FILE(c) ((c) + 0x2F9)
#define CHARA_T(c) (c)
#define CHARA_FILE10(c) ((c) * 10)
#define CHARA_FROM_FILE(id) (((id) - 0x590) / 10)
#define CHARA_VOICE_FILE(c, base, alt) ((c) + (base))
#endif

#endif

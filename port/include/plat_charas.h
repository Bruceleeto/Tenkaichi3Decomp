/* Characters added from outside the disc: one folder per character in `characters` next to the game, holding
   the files of the community's character packs as they come (see plat_charas.c). */
#ifndef PORT_PLAT_CHARAS_H
#define PORT_PLAT_CHARAS_H

#define PORT_CHARA_FIRST 0xA5 /* the same numbers as include/port_chara.h */
#define PORT_CHARA_MAX 27
#define PORT_CHARA_FILE_INDEX 71000  /* in the second archive: ten files per added character */
#define PORT_CHARA_VOICE_INDEX 72000 /* two per added character */
#define PORT_CHARA_FACE_INDEX 73000  /* one per added character: the large picture of the menus */

void PortCharas_Init(void); /* reads the folder; once */
int Port_CharaCount(void);
int Port_CharaBase(int chara);           /* an added character's base character; any other id as it is */
int Port_CharaCostumes(int chara);       /* how many costumes an added character's folder has (0: not an added one) */
const char *Port_CharaName(int index);   /* the folder's name */
/* 1 if the file the game asks for is an added character's; `out` gets the path of the file in its folder, or of
   the base character's file where the folder has none (relative to the data root). */
int PortCharas_Alias(const char *rel, char *out, unsigned n);
/* The folder's chip for the character select (`chip.bin`: one entry of the disc's chip pack, 5,504 bytes), read
   afresh into memory the game can address; NULL if the folder has none. The caller relocates it as it does the
   disc's entries, so it is loaded once per visit of a screen and then asked for with Port_CharaChip. */
void *Port_CharaChipLoad(int index);
void *Port_CharaChip(int chara);
/* The names of a screen with the added characters' after them: a copy of the screen's names pack (`pack`, the
   disc's: a count, offsets, entries of UTF-16 text) with one entry more per added character, from the folder's
   `name.txt` or else the folder's name. Port_CharaNameLine(chara) is an added character's line in it. */
void *Port_CharaNamePack(void *pack);
int Port_CharaNameLine(int chara);

#endif

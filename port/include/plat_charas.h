/* Characters added from outside the disc: one folder per character in `characters` next to the game, holding
   the files of the community's character packs as they come (see plat_charas.c). */
#ifndef PORT_PLAT_CHARAS_H
#define PORT_PLAT_CHARAS_H

#define PORT_CHARA_FIRST 0xA5 /* the same numbers as include/port_chara.h */
#define PORT_CHARA_MAX 27
#define PORT_CHARA_FILE_INDEX 71000  /* in the second archive: ten files per added character */
#define PORT_CHARA_VOICE_INDEX 72000 /* two per added character */
#define PORT_CHARA_FACE_INDEX 73000  /* one per added character: the large picture of the menus */
#define PORT_CHARA_LINE_INDEX 74000  /* two hundred per added character: its spoken lines, a hundred in each language */

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
/* Transformations and fusions between added characters. A pack's own parameter table names its other forms by the
   character numbers of whoever made it; `forms.txt` in the folder says which FOLDER each one is:
       left=Fighter_119        (the four transformation slots: left, up, right, down)
       fusion1=Fighter_104     (the three fusion results: fusion1..fusion3)
       down=disc:3         (or a character of the disc, by its id)
   Port_CharaFormTarget gives the id for a slot (0..3 transformations, 4..6 fusions): the folder's id if it is
   installed, 0xFF (none) if it is not, and -1 where forms.txt says nothing (the table's own number counts). */
int Port_CharaFormTarget(int chara, int slot);
/* For the character select, where the disc shows a character's forms under one cell: the forms of an added
   character, itself first, then those its transformations lead to (through forms.txt), at most `max`. Returns
   their number (1: a character on its own), or -1 if `chara` is itself a form of another installed character and
   has no cell of its own. A character nobody transforms into is the head of its group; "down" is not counted as
   leading to a form (it is how a form goes back). */
int Port_CharaForms(int chara, int *out, int max);
void *Port_CharaNamePack(void *pack);
int Port_CharaNameLine(int chara);

#endif

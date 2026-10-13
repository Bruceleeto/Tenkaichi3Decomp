/* Characters added from outside the disc: one folder per character in `characters` next to the game, holding
   the files of the community's character packs as they come (see plat_charas.c). */
#ifndef PORT_PLAT_CHARAS_H
#define PORT_PLAT_CHARAS_H

#define PORT_CHARA_FIRST 0xC6 /* the same numbers as include/port_chara.h */
#define PORT_CHARA_MAX 57
#define PORT_CHARA_FILE_INDEX 71000  /* in the second archive: ten files per added character */
#define PORT_CHARA_VOICE_INDEX 72000 /* two per added character */
#define PORT_MODEL_MAX 0x180000 /* the largest model file taken (the original's buffer is 0xCE000) */
#define PORT_CHARA_MEMBER_INDEX 91000 /* two per added character: the fight's member block, a side each */
#define PORT_CHARA_FACE_INDEX 90000  /* one per added character: the large picture of the menus */
#define PORT_CHARA_LINE_INDEX 74000  /* two hundred per added character: its spoken lines, a hundred in each language */

void PortCharas_Init(void); /* reads the folder; once */
#define PORT_CHARA_FORMAT 1 /* the package format this game knows (`format=` of character.txt) */
enum { PORT_STAT_HEALTH, PORT_STAT_KI, PORT_STAT_BLAST, PORT_STAT_COUNT };
/* The folders looked at, for the settings window: every folder that is a package or was meant as one, in the
   order they were read (a character's inner folders after it). Also in a session, where none of them is used. */
enum { PORT_PKG_ADDS, PORT_PKG_REPLACES, PORT_PKG_OFF };
typedef struct PortCharaPkg {
    char folder[132];  /* "Name" or "Character/Form" */
    char name[64];     /* the name for the menus */
    char author[64], version[32], about[200]; /* the manifest's; "" where it says nothing */
    char problem[400]; /* what was found wrong with it, "" if nothing */
    int kind;          /* PORT_PKG_ADDS, PORT_PKG_REPLACES, or PORT_PKG_OFF: not used (the problem says why) */
    int id;            /* the id it got (an added character), the disc character it replaces; -1 */
} PortCharaPkg;
int Port_CharaPkgCount(void);
const PortCharaPkg *Port_CharaPkg(int index);
/* A number of a package (`health=`, `ki=`, `blast=`) for a character, or `value` where none is said. */
int Port_CharaStat(int chara, int which, int value);
int PortCharas_Epoch(void); /* changes when a session begins or ends: what a file number answers may change with it */
int Port_CharaCount(void);     /* the added characters; 0 in an online session, as everything here then answers */
int Port_CharaInstalled(void); /* how many the folder holds, in a session too */
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
/* The same for a whole screen: PortChips_Load copies every folder's chip into the game's own memory (a picture
   is fetched by the renderer from the game's memory) and makes it ready as the screen does the disc's;
   PortChips_Get gives an added character's (NULL: none, the base character's is shown); PortChips_Free gives the
   memory back when the screen ends. One screen at a time. */
/* "Added characters in random picks" (setting chara_pool, off unless set; BT3_CHARA_POOL=1): the random cell of
   the select screens and the tournament's computer entrants may be added characters. */
int Port_CharaPool(void);
void Port_CharaPoolSet(int on);
void PortChips_Load(void *pack);
void *PortChips_Get(int chara);
void PortChips_Free(void);
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

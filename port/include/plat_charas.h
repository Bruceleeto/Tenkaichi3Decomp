/* Characters added from outside the disc: one folder per character in `characters` next to the game, holding
   the files of the community's character packs as they come (see plat_charas.c). */
#ifndef PORT_PLAT_CHARAS_H
#define PORT_PLAT_CHARAS_H

#define PORT_CHARA_FIRST 0xA5 /* the same numbers as include/port_chara.h */
#define PORT_CHARA_MAX 27
#define PORT_CHARA_FILE_INDEX 71000  /* in the second archive: ten files per added character */
#define PORT_CHARA_VOICE_INDEX 72000 /* two per added character */

void PortCharas_Init(void); /* reads the folder; once */
int Port_CharaCount(void);
int Port_CharaBase(int chara);           /* an added character's base character; any other id as it is */
int Port_CharaCostumes(int chara);       /* how many costumes an added character's folder has (0: not an added one) */
const char *Port_CharaName(int index);   /* the folder's name */
/* 1 if the file the game asks for is an added character's; `out` gets the path of the file in its folder, or of
   the base character's file where the folder has none (relative to the data root). */
int PortCharas_Alias(const char *rel, char *out, unsigned n);

#endif

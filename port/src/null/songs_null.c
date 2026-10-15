/*
 * Added songs left out (the Dreamcast build): port/src/plat_songs.c, the manifest of songs added to the music
 * select, and port/src/gs/song_mp3.c, which converts MP3 files into the ADX the game plays. The music select lists
 * the disc's tracks alone.
 */
#include <stdlib.h>
#include "plat_songs.h"

char gPortSongNames[PORT_SONG_MAX][64];
int gPortSongCount;
int gPortSongOffsets[PORT_SONG_MAX];

void PortSongs_Init(void) {}
void PortSongs_Refresh(void) {}
int PortSongs_Count(void) { return 0; }
int PortSongs_Offset(int index) { return PORT_SONG_FIRST_OFFSET + index; }
int PortSongs_Alias(const char *rel, char *out, unsigned n) { (void)rel; (void)out; (void)n; return 0; }
int PortSongs_IsFile(const char *path) { (void)path; return 0; }

/* BT3_TEST_BGM=<offset>: the fight plays that music-list entry (as plat_songs.c) */
int Port_TestBgm(void) {
    const char *e = getenv("BT3_TEST_BGM");
    return e != NULL && e[0] != '\0' ? (int)strtol(e, NULL, 0) : -1;
}

/*
 * MP3 songs left out (the Dreamcast build): port/src/gs/song_mp3.c converts MP3 files dropped in the PC's songs
 * folder into the ADX the game plays. A song on the Dreamcast's disc is an ADX already (port/tools/add_song.py).
 */
void PortSongs_Mp3Start(const char *dir) { (void)dir; }
int PortSongs_Mp3Made(void) { return 0; }
int PortSongs_Mp3Busy(void) { return 0; }

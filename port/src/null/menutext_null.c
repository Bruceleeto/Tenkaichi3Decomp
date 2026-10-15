/*
 * The TrueType menu fonts left out (the Dreamcast build): port/src/gs/menu_text.c draws the menus' labels and the
 * game's text again from a font (the PC's F1 > UI settings), menu_labels.c makes the list of labels it needs, and
 * namefont.c letters the names of stages and songs added from outside the disc. Answered as the setting "Original"
 * (font 0): the game's own lettering, untouched. An added stage or song has no name picture.
 */
#include <stdint.h>

int MenuText_FontId(int choice) { (void)choice; return 0; }
int MenuText_RasterScale(int internalScale) { return internalScale < 2 ? 2 : internalScale > 4 ? 4 : internalScale; }
int MenuText_GetFont(void) { return 0; }
void MenuText_SetFont(int choice) { (void)choice; }
void MenuText_SetFontId(int id) { (void)id; }
int MenuText_FontIdToSave(void) { return 0; }
void MenuText_Poll(void) {}
uint8_t *MenuText_Render(const uint8_t *rgba, int width, int height, int indexed, int scale, int *outWidth, int *outHeight) {
    (void)rgba; (void)width; (void)height; (void)indexed; (void)scale; (void)outWidth; (void)outHeight;
    return 0; /* (the game's own sprite) */
}
int MenuText_GetSharedFont(void) { return 0; }
void MenuText_SetSharedFont(int choice) { (void)choice; }
void MenuText_SetSharedFontId(int id) { (void)id; }
uint32_t MenuText_TexKey(int width, int height, int indexed, int tbp, int cbp, int scale) {
    (void)width; (void)height; (void)indexed; (void)tbp; (void)cbp; (void)scale;
    return 0;
}
void MenuText_RegisterGlyphs(int tbp, int cbp, const void *glyphs, int count, int lineHeight) {
    (void)tbp; (void)cbp; (void)glyphs; (void)count; (void)lineHeight;
}

/* namefont.c */
void Port_NameFontSheets(int stage, int song, int songLit) { (void)stage; (void)song; (void)songLit; }

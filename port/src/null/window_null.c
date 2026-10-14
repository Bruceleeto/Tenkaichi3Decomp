/*
 * No window (the Dreamcast: the picture goes to the television): what port/src/gs/gs_window.c does with SDL,
 * done without it. There is nothing to create, resize or make full screen; the wait is the system's.
 */
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "../gs/ui.h"

void GsGpu_TitlePoll(void) {}
struct SDL_Window *GsDraw_WindowCreate(int opengl) { (void)opengl; return NULL; }
struct SDL_Window *GsDraw_Window(void) { return NULL; }
void GsDraw_WindowShape(void) {}
void GsDraw_FullscreenSet(int on) { (void)on; }
void GsDraw_FullscreenToggle(void) {}
int GsDraw_Fullscreen(void) { return 1; }
void GsDraw_Events(void) {}

void GsDraw_Wait(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    nanosleep(&ts, NULL);
}

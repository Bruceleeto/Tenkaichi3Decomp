/*
 * The window (SDL): the one both back ends draw in, its shape, full screen, display and title, and the precise wait
 * of the in-between pictures. What gs_draw.c asks of the system it runs on; port/src/null/window_null.c is the same
 * without a window (the Dreamcast: the picture goes to the television).
 */
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "gs_internal.h"
#include "gs_draw.h"
#include "ui.h"

void Port_SettingSave(const char *name, int value); /* plat_settings.c */
void Port_SettingsWrite(void);
int Port_AspectMilli(void); /* plat_stub.c */

/* The one window both back ends share: shape (aspect), full screen and display are the renderer's, not a
   back end's, so the settings window and F11 work the same whichever back end is running. */
static SDL_Window *sWindow;
/* Called once a frame by the back ends: when the check of the game data (plat_verify.c) has found it modified, the
   window's title says so from then on, so that a screenshot of the window carries it. */
void GsGpu_TitlePoll(void) {
    extern int Port_DataState(void);
    static int done;
    if (!done && sWindow != NULL && Port_DataState() == 2) {
        SDL_SetWindowTitle(sWindow, "Tenkaichi3Decomp [modified game data]");
        done = 1;
    }
}
static int sFullscreen;

/* Created (with SDL's video subsystem) before the device / the GL context: everything the user can choose
   about it (BT3_WINDOW, BT3_DISPLAY, BT3_FULLSCREEN, the saved settings, the aspect ratio) is decided here,
   so both back ends show the same window. `opengl` requests a window a GL context can be made current on. */
SDL_Window *GsDraw_WindowCreate(int opengl) {
    float want = (float)Port_AspectMilli() / 1000.0f;
    int h = 480, w = (int)((float)h * want + 0.5f), count = 0, pick = 0;
    SDL_DisplayID *displays;
    SDL_PropertiesID props;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "bt3: SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    displays = SDL_GetDisplays(&count);
    props = SDL_CreateProperties();
    /* The window has the picture's shape: 480 lines high, 4:3 or 16:9 wide (640 x 480 or 853 x 480), and keeps
       that shape when it is resized. BT3_WINDOW=WxH gives another start size. */
    if (getenv("BT3_WINDOW") != NULL) {
        sscanf(getenv("BT3_WINDOW"), "%dx%d", &w, &h);
    }
    /* BT3_DISPLAY=n: the n-th display (1 = first) for the window or the full screen; without it the desktop
       places the window. Some desktops (Wayland) ignore a window's position but honour the display of a
       full-screen window. BT3_FULLSCREEN=1 starts in borderless full screen; F11 switches. */
    for (pick = 0; pick < count; pick++) {
        SDL_Rect r;
        SDL_GetDisplayBounds(displays[pick], &r);
        fprintf(stderr, "bt3: display %d: %s, %d x %d\n", pick + 1, SDL_GetDisplayName(displays[pick]), r.w, r.h);
    }
    pick = getenv("BT3_DISPLAY") != NULL ? atoi(getenv("BT3_DISPLAY")) : Port_Setting("display", 0);
    sFullscreen = (getenv("BT3_FULLSCREEN") != NULL ? atoi(getenv("BT3_FULLSCREEN")) : Port_Setting("fullscreen", 0)) != 0;
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Tenkaichi3Decomp");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    if (opengl) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    }
    if (pick >= 1 && pick <= count) {
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(displays[pick - 1]));
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(displays[pick - 1]));
    } else if (pick != 0) {
        fprintf(stderr, "bt3: BT3_DISPLAY=%d: there are %d displays\n", pick, count);
    }
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, sFullscreen);
    sWindow = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_free(displays);
    if (sWindow != NULL && !sFullscreen) {
        SDL_SetWindowSize(sWindow, w, h);
    }
    {
        int ww = 0, wh = 0;
        if (sWindow != NULL) { SDL_GetWindowSize(sWindow, &ww, &wh); }
        fprintf(stderr, "bt3: internal resolution %dx (%d x %d), window %d x %d\n", gsScale, 512 * gsScale, 448 * gsScale, ww, wh);
    }
    return sWindow;
}

SDL_Window *GsDraw_Window(void) { return sWindow; }

void GsDraw_WindowShape(void) {
    float want = (float)Port_AspectMilli() / 1000.0f;
    int w, h;
    if (!sFullscreen && sWindow != NULL) {
        SDL_GetWindowSize(sWindow, &w, &h);
        SDL_SetWindowSize(sWindow, (int)((float)h * want + 0.5f), h);
    }
}

void GsDraw_FullscreenSet(int on) {
    if (sWindow == NULL) {
        return;
    }
    sFullscreen = on;
    SDL_SetWindowFullscreen(sWindow, on);
}

void GsDraw_FullscreenToggle(void) {
    GsDraw_FullscreenSet(!sFullscreen);
    Port_SettingSave("fullscreen", sFullscreen);
    Port_SettingsWrite();
}

int GsDraw_Fullscreen(void) { return sFullscreen; }

/* ns from now, to within a fraction of a millisecond (gs_draw.c: when an in-between picture is due) */
void GsDraw_Wait(uint64_t ns) { SDL_DelayPrecise(ns); }

/* The window's events, once a frame (the PVR back end): closing it or Esc ends the program, F11 switches full
   screen; the screen reader's repeat key and the settings window's hot key see them first. */
void GsDraw_Events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) { exit(0); }
        { extern void PortA11y_Event(const SDL_Event *ev); PortA11y_Event(&ev); } /* a11y.c: the repeat key and button */
        if (PortInput_Hotkey(&ev)) { continue; } /* (no settings window on this back end yet) */
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !ev.key.repeat) { exit(0); }
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_F11) { GsDraw_FullscreenToggle(); }
    }
}

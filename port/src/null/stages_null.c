/*
 * Added stages left out (the Dreamcast build): port/src/plat_stages.c, the manifest of stages added from outside
 * the disc. The menus list the disc's stages alone, and a stage's model buffer is the game's own size. The test
 * settings it also holds are kept.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat_stages.h"

#define PORT_STAGE_BUF_GAME 0x6CB800 /* the game's stage buffer (as plat_stages.c) */

char gPortStageNames[PORT_STAGE_MAX][64];
int gPortExtraStageCount;
int gPortExtraStages[PORT_STAGE_MAX];

void PortStages_Init(void) {}
int PortStages_Count(void) { return 0; }
const char *PortStages_Name(int index) { (void)index; return ""; }
int PortStages_Alias(const char *rel, char *out, unsigned n) { (void)rel; (void)out; (void)n; return 0; }

int Port_StageBufSize(void) { return PORT_STAGE_BUF_GAME; }

/* A stage file about to be read: 0 if it does not fit the buffer (the disc's stage models: 368..402 for one
   screen, 407..441 split). */
int Port_StageFits(const char *rel, long size) {
    int index = -1;
    if (strncmp(rel, "pzs3us1/", 8) != 0 || sscanf(rel + 8, "%d", &index) != 1) {
        return 1;
    }
    if ((index >= 368 && index <= 402) || (index >= 407 && index <= 441)) {
        return size <= PORT_STAGE_BUF_GAME;
    }
    return 1;
}

/* The test settings (as plat_stages.c): BT3_TEST_STAGE, BT3_TEST_SCREEN, BT3_TEST_MOVIE, BT3_TEST_DEMO_HUD. */
int Port_TestStage(void) {
    const char *e = getenv("BT3_TEST_STAGE");
    return e != NULL ? (int)strtol(e, NULL, 0) : -1;
}
int Port_TestScreen(void) {
    const char *e = getenv("BT3_TEST_SCREEN");
    return e != NULL ? (int)strtol(e, NULL, 0) : -1;
}
int Port_TestMovie(void) {
    const char *e = getenv("BT3_TEST_MOVIE");
    return e == NULL ? 0 : (e[0] == 'e' || e[0] == 'E') ? 2 : 1;
}
int Port_TestDemoHud(void) {
    static int on = -1;
    if (on < 0) { on = getenv("BT3_TEST_DEMO_HUD") != NULL && atoi(getenv("BT3_TEST_DEMO_HUD")) != 0; }
    return on;
}

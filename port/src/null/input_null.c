/*
 * No controllers yet (the Dreamcast: its pads through maple are still to come): what port/src/gs/gs_input.c does
 * with SDL, without it. No pad is read (plat_stub.c gives the game one with nothing pressed), nothing vibrates, the
 * settings are their defaults.
 */
int Port_PadRead(int socket, unsigned char *data) { (void)socket; (void)data; return 0; }
void Port_PadVibrate(int socket, int small, int large) { (void)socket; (void)small; (void)large; }
int Port_RumbleTest(void) { return 0; }
void PortInput_Fresh(void) {}
int Port_ClashSquare(void) { return 0; }
void Port_ClashLog(int player, const char *what, int fromInput, int total, int frame) {
    (void)player; (void)what; (void)fromInput; (void)total; (void)frame;
}
float PortA11y_StreamGain(const void *player) { (void)player; return 1.0f; } /* (a11y.c's: no screen reader) */
int gPortOverlayOpen; /* (the settings window: there is none) */

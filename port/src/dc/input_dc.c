/*
 * The Dreamcast's pads through maple: what port/src/gs/gs_input.c does with SDL, the PS2's pad bytes from a KOS
 * controller (socket 0 the first controller found, socket 1 the second). The face buttons where the PS2's are
 * (A Cross, B Circle, X Square, Y Triangle), Start, the d-pad and the stick as the left one; a trigger half in is
 * L1/R1, all the way in L2/R2 as well. Nothing vibrates yet, the settings are their defaults.
 */
#include <string.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

/* bits of the two button bytes together (byte 0 low), as include/sys/pad.h has them; 1 = pressed here */
enum { B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040,
       B_LEFT = 0x0080, B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000,
       B_CROSS = 0x4000, B_SQUARE = 0x8000 };

#define TRIG_HALF 40  /* a trigger past this: L1 / R1 */
#define TRIG_FULL 200 /* and past this: L2 / R2 as well */

static unsigned char stick(int v) { /* KOS's -128..127 to the PS2's 0..255, 0x80 the middle */
    v += 128;
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

int Port_PadRead(int socket, unsigned char *data) {
    maple_device_t *dev = maple_enum_type(socket, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    unsigned btn = 0;
    if (dev == NULL || (st = (cont_state_t *)maple_dev_status(dev)) == NULL)
        return 0;
    if (st->buttons & CONT_A) btn |= B_CROSS;
    if (st->buttons & CONT_B) btn |= B_CIRCLE;
    if (st->buttons & CONT_X) btn |= B_SQUARE;
    if (st->buttons & CONT_Y) btn |= B_TRIANGLE;
    if (st->buttons & CONT_START) btn |= B_START;
    if (st->buttons & CONT_DPAD_UP) btn |= B_UP;
    if (st->buttons & CONT_DPAD_DOWN) btn |= B_DOWN;
    if (st->buttons & CONT_DPAD_LEFT) btn |= B_LEFT;
    if (st->buttons & CONT_DPAD_RIGHT) btn |= B_RIGHT;
    if (st->buttons & CONT_Z) btn |= B_SELECT; /* (the arcade sticks' extra buttons) */
    if (st->buttons & CONT_C) btn |= B_R3;
    if (st->buttons & CONT_D) btn |= B_L3;
    if (st->ltrig > TRIG_HALF) btn |= B_L1;
    if (st->ltrig > TRIG_FULL) btn |= B_L2;
    if (st->rtrig > TRIG_HALF) btn |= B_R1;
    if (st->rtrig > TRIG_FULL) btn |= B_R2;

    memset(data, 0, 18);
    data[0] = (unsigned char)(~btn & 0xFF); /* active low */
    data[1] = (unsigned char)(~(btn >> 8) & 0xFF);
    data[2] = stick(st->joy2x); /* right stick: the second one, where a pad has it; 0x80 if not */
    data[3] = stick(st->joy2y);
    data[4] = stick(st->joyx);
    data[5] = stick(st->joyy);
    return 18;
}

void Port_PadVibrate(int socket, int small, int large) { (void)socket; (void)small; (void)large; }
int Port_RumbleTest(void) { return 0; }
void PortInput_Fresh(void) {}
int Port_ClashSquare(void) { return 0; }
void Port_ClashLog(int player, const char *what, int fromInput, int total, int frame) {
    (void)player; (void)what; (void)fromInput; (void)total; (void)frame;
}
float PortA11y_StreamGain(const void *player) { (void)player; return 1.0f; } /* (a11y.c's: no screen reader) */
int gPortOverlayOpen; /* (the settings window: there is none) */

#ifndef SYS_HWREG_H
#define SYS_HWREG_H

#include "types.h"

/* The PS2 hardware registers the game touches (the EE's timers, DMA and VIF/GIF registers at 0x10000000, the GS's
   display registers at 0x12000000). The port has none: a transfer is a call (sys/dma.c: Port_GsVif1Chain,
   Port_GsGifChannel), the timers are numbers of sys/timer.c's own, and the display registers are not written. (On
   the Dreamcast the PS2 addresses are the tile accelerator's input.) */
#ifndef PORT
#define HW_EE(a) ((volatile u32 *)(a))
#define HW_GS(a) ((volatile u64 *)(a))
#endif

#endif

#ifndef SYS_HWREG_H
#define SYS_HWREG_H

#include "types.h"

/* The PS2 hardware registers the game touches (the EE's timers, DMA and VIF/GIF registers at 0x10000000, the GS's
   display registers at 0x12000000), as plain memory at the same offsets: a read gives what was last written, and
   the port's DMA code (port/src/plat_gs.c) reads what the game wrote. Nothing lives at the PS2 addresses
   themselves, which on the Dreamcast are the tile accelerator's input. */
extern volatile u32 gHwEe[0x10000 / 4]; /* 0x10000000..0x1000FFFF */
extern volatile u64 gHwGs[0x2000 / 8];  /* 0x12000000..0x12001FFF */

#define HW_EE(a) (&gHwEe[((a) - 0x10000000u) / 4])
#define HW_GS(a) (&gHwGs[((a) - 0x12000000u) / 8])

#endif

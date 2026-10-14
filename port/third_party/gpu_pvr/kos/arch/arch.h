/* Host stand-in for KOS <arch/arch.h>: PVR_RAM_SIZE asks for the system type. */
#ifndef __ARCH_ARCH_H
#define __ARCH_ARCH_H
#include <stdint.h>
#define HW_TYPE_RETAIL 0x0
#define HW_TYPE_SET5   0x1
/* SET5 (the 16 MB VRAM dev kit), to match kos_pvr.cpp's VRAM_SIZE: as RETAIL, PVR_RAM_SIZE is 8 MB and
   to_pvr_txr_ptr masks every texture address to it, so a texture in the upper 8 MB was read from 8 MB lower. */
static inline int hardware_sys_mode(int *region) { if (region) *region = 0; return HW_TYPE_SET5; }
#endif

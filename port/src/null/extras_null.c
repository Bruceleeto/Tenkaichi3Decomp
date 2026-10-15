/*
 * The PC's extras left out (the Dreamcast build): the screen reader's game side (port/src/a11y_game.c; its speaker,
 * gs/a11y.c, is the PC's only) and the heap report (port/src/plat_heapstat.c). The heap keeps BT3_HEAP_MB.
 */
#include <stdint.h>
#include <stdlib.h>

void PortA11y_FontText(int32_t kind, int32_t arg, int32_t flags) { (void)kind; (void)arg; (void)flags; }
void PortA11y_GameTick(void) {}
void PortA11y_StreamStart(const char *path) { (void)path; }

int Port_HeapCap(int size) {
    const char *mb = getenv("BT3_HEAP_MB");
    return mb != NULL && atoi(mb) > 0 ? atoi(mb) << 20 : size;
}
void Port_HeapCreated(uint32_t start, int32_t size) { (void)start; (void)size; }
void Port_HeapNoteAlloc(uint32_t data, int32_t size, uint32_t align, int32_t fromTail, uint32_t block, int32_t blockSize, void *caller) {
    (void)data; (void)size; (void)align; (void)fromTail; (void)block; (void)blockSize; (void)caller;
}
void Port_HeapNoteFree(uint32_t data) { (void)data; }

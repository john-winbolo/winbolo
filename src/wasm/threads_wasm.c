/*
 * threads_wasm.c - No-op thread manager for WASM build
 *
 * Replaces server/threads.c. In WASM there are no server threads;
 * all game logic runs on the main thread inside emscripten_set_main_loop.
 */

#include "../bolo/global.h"
#include "../server/threads.h"

bool threadsCreate(bool context)  { (void)context; return TRUE; }
void threadsDestroy(void)         { }
void threadsWaitForMutex(void)    { }
bool threadsTryWaitForMutex(void) { return TRUE; }
void threadsReleaseMutex(void)    { }

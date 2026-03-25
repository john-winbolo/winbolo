/*
 * clientmutex_wasm.c - No-op mutex for single-threaded WASM build
 */

#include "../bolo/global.h"
#include "../gui/clientmutex.h"

bool clientMutexCreate(void)    { return TRUE; }
void clientMutexDestroy(void)   { }
void clientMutexWaitFor(void)   { }
bool clientMutexTryWaitFor(void){ return TRUE; }
void clientMutexRelease(void)   { }

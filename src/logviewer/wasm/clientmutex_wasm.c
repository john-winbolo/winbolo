/*
 * clientmutex_wasm.c - No-op mutex for single-threaded WASM build
 *
 * Replaces clientmutex.c — WASM is single-threaded so no
 * synchronization is needed. All functions are no-ops.
 */

#include "global.h"
#include "clientmutex.h"

bool clientMutexCreate(void) {
    return TRUE;
}

void clientMutexDestroy(void) {
}

void clientMutexWaitFor(void) {
}

void clientMutexRelease(void) {
}

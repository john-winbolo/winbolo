/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * clientmutex_wasm.c - No-op mutex for single-threaded WASM build
 *
 * Replaces clientmutex.c — WASM is single-threaded so no
 * synchronization is needed. All functions are no-ops.
 */

#include "lv_global.h"
/* Spelled out: src/gui/clientmutex.h carries the same _CLIENT_MUTEX_H guard
 * and declares the client's own clientMutex* API. It sits ahead of
 * src/logviewer on the wasm game client's include path, so an unqualified
 * "clientmutex.h" reaches the wrong world from this directory. */
#include "../clientmutex.h"

bool lv_clientMutexCreate(void) {
    return TRUE;
}

void lv_clientMutexDestroy(void) {
}

void lv_clientMutexWaitFor(void) {
}

void lv_clientMutexRelease(void) {
}

/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * clientmutex_wasm.c - No-op mutex for single-threaded WASM build
 *
 * Replaces clientmutex.c — WASM is single-threaded so no
 * synchronization is needed. All functions are no-ops.
 */

#include "lv_global.h"
#include "clientmutex.h"

bool lv_clientMutexCreate(void) {
    return TRUE;
}

void lv_clientMutexDestroy(void) {
}

void lv_clientMutexWaitFor(void) {
}

void lv_clientMutexRelease(void) {
}

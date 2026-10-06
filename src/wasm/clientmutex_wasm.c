/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * clientmutex_wasm.c - No-op client mutex for the WASM build
 *
 * Only the page's thread takes this mutex. Bot thinks run on worker threads
 * but never take it, so it stays a no-op.
 */

#include "global.h"
#include "../gui/clientmutex.h"

bool clientMutexCreate(void)    { return TRUE; }
void clientMutexDestroy(void)   { }
void clientMutexWaitFor(void)   { }
bool clientMutexTryWaitFor(void){ return TRUE; }
void clientMutexRelease(void)   { }

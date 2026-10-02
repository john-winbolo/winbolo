/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * threads_wasm.c - No-op thread manager for WASM build
 *
 * Replaces server/threads.c. In WASM there is no server thread: the server
 * ticks on the page's thread. Bot thinks run on worker threads, but they
 * never take this mutex; only the page's thread does, so it stays a no-op.
 */

#include "global.h"
#include "threads.h"

bool threadsCreate(bool context)  { (void)context; return TRUE; }
void threadsDestroy(void)         { }
void threadsWaitForMutex(void)    { }
bool threadsTryWaitForMutex(void) { return TRUE; }
void threadsReleaseMutex(void)    { }

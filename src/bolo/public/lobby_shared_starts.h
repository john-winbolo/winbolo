/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LOBBY_SHARED_STARTS_H
#define LOBBY_SHARED_STARTS_H

/* Several players may reserve the same lobby start (the spawn scatter
 * places them a few squares apart). Set to 0 to restore one player per
 * start everywhere: server rules, batch placement, and every lobby surface.
 *
 * Every site tests the flag with the runtime helper below rather than
 * #if, so both the shared-start and the one-holder code path always
 * compile and neither can rot. The compiler folds the constant away, so
 * the build costs nothing either way. */

#include <stdbool.h>

#define LOBBY_SHARED_STARTS 1

static inline bool lobbySharedStartsEnabled(void) {
    return LOBBY_SHARED_STARTS != 0;
}

#endif /* LOBBY_SHARED_STARTS_H */

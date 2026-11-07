/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHELL_LGM_REWIND_H
#define SHELL_LGM_REWIND_H

/* How much shorter a player's shell rewinds builders than tanks, in rewind
 * entries (one entry is one 20 ms game tick). The reason is beside
 * shellsBlastTestBuilder in src/bolo/shells.c: the shooter's client draws
 * builders from a snapshot one tick newer than the one it draws tanks at.
 *
 * Header only, with no includes, so the server and the Log Viewer's lag view
 * use the same rule. */
#define SHELL_LGM_REWIND_LESS 1

/* The rewind a builder is tested at for a shell whose tank rewind is
 * rewind: SHELL_LGM_REWIND_LESS less, but only from a rewind longer than
 * that, so it never takes a rewind down to nothing. */
static inline unsigned shellLgmRewind(unsigned rewind) {
  return rewind > SHELL_LGM_REWIND_LESS ? rewind - SHELL_LGM_REWIND_LESS : rewind;
}

#endif /* SHELL_LGM_REWIND_H */

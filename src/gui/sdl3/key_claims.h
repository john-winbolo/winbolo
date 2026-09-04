/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef WINBOLO_KEY_CLAIMS_H
#define WINBOLO_KEY_CLAIMS_H

#include <stdbool.h>

#include "../input.h"   /* keyItems */

/* True when `scancode` is bound to an in-game action. A window that also
   drives the game must leave such a key alone: the player's binding wins
   and the window does without it. Scancode 0 means "unbound", and is never
   claimed, so an unset binding cannot swallow a key.

   Every field of keyItems is compared — a field added there and not added
   here is a key the game owns and a second window silently steals. */
static inline bool keyIsClaimedByGame(const keyItems *k, int scancode) {
    if (!k || scancode <= 0) return false;
    return scancode == k->kiForward      || scancode == k->kiBackward     ||
           scancode == k->kiLeft         || scancode == k->kiRight        ||
           scancode == k->kiShoot        || scancode == k->kiLayMine      ||
           scancode == k->kiGunIncrease  || scancode == k->kiGunDecrease  ||
           scancode == k->kiTankView     || scancode == k->kiPillView     ||
           scancode == k->kiOverviewZoom   || scancode == k->kiOverviewFollow  ||
           scancode == k->kiOverviewZoomIn || scancode == k->kiOverviewZoomOut ||
           scancode == k->kiScrollUp     || scancode == k->kiScrollDown   ||
           scancode == k->kiScrollLeft   || scancode == k->kiScrollRight  ||
           scancode == k->kiAllyView     || scancode == k->kiLGMView      ||
           scancode == k->kiBaseView     || scancode == k->kiQuickTree    ||
           scancode == k->kiQuickRoad    || scancode == k->kiQuickWall    ||
           scancode == k->kiQuickPillbox || scancode == k->kiQuickMine;
}

#endif /* WINBOLO_KEY_CLAIMS_H */

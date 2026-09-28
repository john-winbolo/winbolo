/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * alliance_enums.h
 *
 * Pure enum definitions for pillbox/base ownership classifications.
 * Carved out of pillbox.h / bases.h so consumers that only need the
 * enum types (e.g., status-bar renderers, frontend signatures) don't
 * have to pull in the internal pillbox/bases struct layouts.
 *
 * T4 leaf — no #includes, no sim coupling.
 */

#ifndef ALLIANCE_ENUMS_H
#define ALLIANCE_ENUMS_H

/* Determines the pill type, good, neutral or evil */
typedef enum {
  pillDead,
  pillAllie,
  pillGood,
  pillNeutral,
  pillEvil,
  pillTankGood,
  pillTankAllie,
  pillTankEvil,
  /* A slot inside the map's pill count whose pillbox a scenario took off
     the map. The panel leaves its place empty, as it does for a slot past
     the count. Last, so the values above keep their numbers. */
  pillOffMap
} pillAlliance;

/* Determines the base type, good, netral or evil */
typedef enum {
  baseDead,
  baseOwnGood,
  baseAllieGood,
  baseNeutral,
  baseEvil
} baseAlliance;

#endif /* ALLIANCE_ENUMS_H */

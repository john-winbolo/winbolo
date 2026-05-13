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
  pillTankEvil
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

/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


/*********************************************************
*Name:          Starts 
*Filename:      starts.h 
*Author:        John Morrison
*Creation Date: 28/10/98
*Last Modified: 24/07/04
*Purpose:
*  Provides operations on player starts 
*********************************************************/

#ifndef STARTS_H
#define STARTS_H


/* Includes */
#include "global.h"
#include "types.h"

/* Spawn safety. Each of these is the classic default of a start_* rule;
 * startsGetStart and the calls under it read the rule, not the constant.
 * Here rather than in starts.c so simRulesClassic can see them. */

/* Distance thresholds in map squares */
#define START_TANK_RANGE 1
#define START_PILL_RANGE 9
#define START_BASE_RANGE 9
/* Minimum distance a scattered spawn keeps from another live tank */
#define START_SPAWN_SEPARATION 2
/* Maximum spiral search steps */
#define START_SCATTER_MAX 1000
/* Fraction of neutral bases before we treat neutral same as own */
#define START_NEUTRAL_THRESHOLD_PCT 20

#define START_TIMES_16 16
#define START0 0
#define START1 1
#define START2 2
#define START3 3
#define START4 4
#define START5 5
#define START6 6
#define START7 7
#define START8 8
#define START9 9
#define START10 10
#define START11 11
#define START12 12
#define START13 13
#define START14 14
#define START15 15

/* Prototypes */

/*********************************************************
*NAME:          startsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Creates and initilises the player starts structure.
*  Sets number of starts to zero
*
*ARGUMENTS:
*  value - Pointer to the starts structure 
*********************************************************/
void startsCreate(starts *value);

/*********************************************************
*NAME:          startsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Destroys the starts data structure. Also frees memory.
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
void startsDestroy(starts *value);

/*********************************************************
*NAME:          startsSetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets the number of starts in the structure 
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  numStarts - The number of starts 
*********************************************************/
void startsSetNumStarts(starts *value, BYTE numStarts);

/*********************************************************
*NAME:          startsGetNumStarts
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Gets the number of starts in the structure
*
*ARGUMENTS:
*  value  - Pointer to the starts structure
*********************************************************/
BYTE startsGetNumStarts(starts *value);

/*********************************************************
*NAME:          startsGetNumActive
*PURPOSE:
*  Returns how many starts are on the map: the slots under
*  the count whose live flag is set. The count itself is
*  the slot count and keeps a removed start's number.
*
*ARGUMENTS:
*  value - Pointer to the starts structure
*********************************************************/
BYTE startsGetNumActive(starts *value);

/*********************************************************
*NAME:          startsSetStart
*AUTHOR:        John Morrison
*CREATION DATE: 28/10/98
*LAST MODIFIED: 28/10/98
*PURPOSE:
*  Sets a specific start with its item data
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void startsSetStart(starts *value, start *item, BYTE startNum);

/*********************************************************
*NAME:          startsAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Puts a start into the list and returns its number in
*  outStartNum. The lowest removed slot is reused; when
*  every slot in the count is live the list is extended and
*  the count raised. Returns FALSE with outStartNum
*  untouched when all MAX_STARTS starts are live.
*
*ARGUMENTS:
*  value       - Pointer to the starts structure
*  item        - The start to store
*  outStartNum - Receives the start number, 1 based
*********************************************************/
bool startsAddItem(starts *value, const start *item, BYTE *outStartNum);

/*********************************************************
*NAME:          startsInstallItem
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Writes a start at the number it is given and marks that
*  slot live, whatever the slot held before. A number past
*  the count raises the count to cover it and leaves every
*  slot the gap opens up removed: a number arrives from a
*  list that has already filled it, so the gap is the set of
*  starts this list has not been told about. Returns FALSE
*  for number 0 or a number past MAX_STARTS.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - The start to store
*  startNum - The start number, 1 based
*********************************************************/
bool startsInstallItem(starts *value, const start *item, BYTE startNum);

/*********************************************************
*NAME:          startsRemoveItem
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Clears a start's live flag. The slot, the count and every
*  start number above it are left alone, so the numbers the
*  map blob and the recordings use keep meaning the same
*  start. Returns FALSE for a number out of range or one
*  already removed.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  startNum - The start number, 1 based
*********************************************************/
bool startsRemoveItem(starts *value, BYTE startNum);

/*********************************************************
*NAME:          startsIsActive
*AUTHOR:        John Morrison
*CREATION DATE: 11/9/26
*LAST MODIFIED: 11/9/26
*PURPOSE:
*  Returns whether a start number names a start that is on
*  the map. A removed start keeps its slot and its number, so
*  a number in range is not on its own enough. A number out
*  of range returns FALSE.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  startNum - The start number, 1 based
*********************************************************/
bool startsIsActive(starts *value, BYTE startNum);

/*********************************************************
*NAME:          startsSetActive
*AUTHOR:        John Morrison
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
*  Puts a start on the map or takes it off it, leaving its
*  record alone either way. The flag is all that moves, so a
*  start put back is the one the slot already held. Returns
*  FALSE for a number out of range.
*
*ARGUMENTS:
*  value   - Pointer to the starts structure
*  startNum - The start number, 1 based
*  onMap   - TRUE for on the map, FALSE for off it
*********************************************************/
bool startsSetActive(starts *value, BYTE startNum, bool onMap);

/*********************************************************
*NAME:          startsExistPos
*AUTHOR:        John Morrison
*CREATION DATE: 2/7/00
*LAST MODIFIED: 2/7/00
*PURPOSE:
*  Returns if a start exists at a specific location
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  xValue   - X Location to test
*  yValue   - Y Location to test
*********************************************************/
bool startsExistPos(starts *value, BYTE xValue, BYTE yValue);

/*********************************************************
*NAME:          startsGetStart
*AUTHOR:        John Morrison
*CREATION DATE: 9/2/98
*LAST MODIFIED: 9/2/98
*PURPOSE:
*  Gets a specific start.
*
*ARGUMENTS:
*  value    - Pointer to the starts structure
*  item     - Pointer to a player start 
*  startNum - The start number
*********************************************************/
void startsGetStartStruct(starts *value, start *item, BYTE startNum);

/*********************************************************
*NAME:          startsGetStart
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 9/4/01
*PURPOSE:
*  Returns a random start position
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  x         - X Value of the start
*  y         - Y Value of the start
*  dir       - Direction it is facing
*  playerNum - Player Number requesting start
*********************************************************/
struct GameSim;
void startsGetStart(struct GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir, BYTE playerNum);

/*********************************************************
*NAME:          startsAssignBatch
*PURPOSE:
*  Computes start positions for every connected player in a
*  single pass, used at the lobby->game transition where the
*  per-player algorithm cannot see siblings being created in
*  the same batch. Players sharing a non-zero teamNumber are
*  grouped near each other (anchored by owned bases when the
*  map encodes them, otherwise stripe-partitioned along the
*  map's long axis). Solo players (teamNumber == 0) are then
*  slotted in via farthest-first to maximise spacing.
*
*  Writes the chosen start index per slot into outStartIdx
*  (0..MAX_STARTS-1) or MAX_STARTS for slots that could not
*  be placed. Scatter and direction conversion are deferred
*  to startsGetStart, which consumes the slot lazily so the
*  per-square nudge sees siblings already created earlier
*  in the batch loop.
*
*  reservedStartIdx0 (optional, may be NULL) carries a 0-based
*  pre-reserved start per slot (MAX_STARTS = none). A connected
*  slot with a valid reservation locks that exact start: it is
*  excluded from the cluster/farthest-first placement and emitted
*  at its reserved index, leaving only the unreserved slots to
*  fill the remaining free starts. Duplicate reservations are
*  all honored when LOBBY_SHARED_STARTS is on — every slot that
*  named the start comes out on it and the spawn scatter spreads
*  them — and honor only the first when it is off, the rest
*  falling through to ordinary placement. An out-of-range
*  reservation is treated as none.
*  NULL means no reservations (original behaviour).
*
*  teamStartSide (optional, may be NULL) is indexed by team
*  number 0..MAX_TANKS (entry 0 unused) and holds each team's
*  START_SIDE_* choice. A team with a side is anchored on that
*  side and takes only starts its side accepts; a chosen side
*  is closed to the teams that did not choose it and to solos.
*  NULL means no sides anywhere (every team START_SIDE_ANY).
*
*  A connected slot the placement leaves without a start of
*  its own rides one already taken — a teammate's where it has
*  one — so two slots may share an index; the tank-aware
*  scatter spreads them out at spawn. MAX_STARTS is emitted
*  only when no start on the map is valid.
*********************************************************/
void startsAssignBatch(struct GameSim *sim, starts *value,
                       const bool *connected, const BYTE *teamNumber,
                       BYTE *outStartIdx, const BYTE *reservedStartIdx0,
                       const BYTE *teamStartSide);

/* One anchor per team, indexed by team number 1..MAX_TANKS (entry 0
 * unused). An anchor is the point on the map a team is placed around, and
 * it is what gives a team a region of its own rather than whatever the
 * other teams left over. A side names a region directly; a team with no
 * side is given the part of the map farthest from every team already
 * anchored, so three teams where only one named a side still get three
 * separate regions instead of sharing one.
 *
 * inUse is FALSE when no team named a side at all. Sides are what put a
 * team anywhere in particular, so with none in play there is nothing to
 * anchor against and both placement paths keep the behaviour they have
 * when no side is chosen: teams cluster where the map's own regions put
 * them. */
typedef struct {
    bool inUse;
    bool present[MAX_TANKS + 1];
    int  x[MAX_TANKS + 1];
    int  y[MAX_TANKS + 1];
} StartsTeamAnchors;

/*********************************************************
*NAME:          startsComputeTeamAnchors
*PURPOSE:
*  Works out where each present team belongs on the map and
*  writes it to out. teamSide and teamPresent are indexed by
*  team number 1..MAX_TANKS, entry 0 unused.
*
*  A team that named a side anchors at the centroid of the
*  usable starts that side accepts, falling back to the
*  centre band and then the bounding box centre. A team with
*  no side anchors at the usable start, among those the
*  chosen sides leave open to it, farthest from every anchor
*  already placed; teams are anchored in team-number order so
*  every peer computes the same answer.
*
*  Returns out->inUse: FALSE when no present team named a
*  side, and then no anchor is written and the caller keeps
*  its own behaviour.
*
*ARGUMENTS:
*  sim         - Game simulation, for start validity
*  value       - Starts structure
*  teamSide    - [MAX_TANKS + 1] START_SIDE_* per team
*  teamPresent - [MAX_TANKS + 1] whether the team has players
*  out         - Receives the anchors
*********************************************************/
bool startsComputeTeamAnchors(struct GameSim *sim, starts *value,
                              const BYTE *teamSide, const bool *teamPresent,
                              StartsTeamAnchors *out);

/*********************************************************
*NAME:          startsAnchorOwns
*PURPOSE:
*  Whether the start at 0-based idx falls in team tn's
*  region: that is, whether tn's anchor is the nearest of
*  every present team's. Ties go to the lower team number so
*  the answer is the same on every peer. Always TRUE when the
*  anchors are not in use, so a caller can rank by region
*  without testing inUse itself.
*********************************************************/
bool startsAnchorOwns(starts *value, const StartsTeamAnchors *anchors,
                      BYTE tn, BYTE idx);

/*********************************************************
*NAME:          startsPickIncremental
*PURPOSE:
*  Picks one free start for a single joiner, 0-based in/out.
*  taken[] is 0-based per start (TRUE = already reserved).
*  teammateStarts0[] lists the 0-based start indices reserved
*  by the joiner's teammates (teammateCount may be 0).
*
*  anchors (optional, may be NULL) gives every present team a
*  region of its own and myTeam names the joiner's. Starts in
*  the joiner's own region come before the rest, above the
*  tiers below: that is what keeps two teams sharing a side,
*  or two teams left the same part of the map, off each
*  other's ground, which eligibility cannot do because it
*  hands both of them the same set. NULL, or anchors not in
*  use, leaves every start in the first pass.
*
*  side is the joiner's team START_SIDE_*; closedMask is the
*  union of the START_SIDE_BIT_* the other teams chose. Only
*  starts startSideEligible allows are candidates, ranked in
*  tiers: starts on the joiner's side alone, starts on its
*  side another team's side also covers, then the centre;
*  the first tier holding a free start wins. With no side
*  every candidate is in the first tier.
*
*  Within a tier: with teammate reservations the side
*  accepts, returns the free valid start with the smallest
*  distance to the nearest of them (cluster); otherwise
*  returns the free valid start maximising the min distance
*  to every taken start (farthest-first). Returns MAX_STARTS
*  when no free valid start the rules allow exists.
*********************************************************/
BYTE startsPickIncremental(struct GameSim *sim, starts *value,
                           const bool *taken,
                           const BYTE *teammateStarts0, int teammateCount,
                           BYTE side, BYTE closedMask,
                           const StartsTeamAnchors *anchors, BYTE myTeam);

/*********************************************************
*NAME:          startsGetRandStart
*AUTHOR:        John Morrison
*CREATION DATE: 7/1/99
*LAST MODIFIED: 9/4/01
*PURPOSE:
*  Returns a random start position. Was the original
*  startsGetStart() code
*
*ARGUMENTS:
*  value     - Pointer to the starts structure
*  x         - X Value of the start
*  y         - Y Value of the start
*  dir       - Direction it is facing
*********************************************************/ 
void startsGetRandStart(struct GameSim *sim, starts *value, BYTE *x, BYTE *y, TURNTYPE *dir);

/*********************************************************
*NAME:          startsConvertDir
*AUTHOR:        John Morrison
*CREATION DATE: 16/1/99
*LAST MODIFIED: 16/1/99
*PURPOSE:
*  Converts a Bolo Map start (0 =East, 4 = North) to my
*  starts 0 = North, 4 = East etc.
*
*ARGUMENTS:
*  dir   - Direction it is facing
*********************************************************/
BYTE startsConvertDir(BYTE dir);

/*********************************************************
*NAME:          startsSetStartNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Sets the starts data to buff from the network.
*
*ARGUMENTS:
*  value   - Pointer to the starts structure
*  buff    - Buffer of data to set starts structure to
*  dataLen - Length of the data
*********************************************************/
void startsSetStartNetData(starts *value, BYTE *buff, BYTE dataLen);

/*********************************************************
*NAME:          startsGetStartNetData
*AUTHOR:        John Morrison
*CREATION DATE: 27/02/99
*LAST MODIFIED: 24/07/04
*PURPOSE:
* Prepares the starts data for sending across the network
* Returns data length
*ARGUMENTS:
*  value - Pointer to the starts structure
*  buff  - Buffer to hold copy of starts data
*********************************************************/
BYTE startsGetStartNetData(starts *value, BYTE *buff);

/*********************************************************
*NAME:          startsGetMaxs
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Gets the maximum positions values of all the starts.
*  eg left most, right most etc.
*
*ARGUMENTS:
*  value  - Pointer to the starts structure
*  left   - Pointer to hold the left most value
*  right  - Pointer to hold the right most value
*  top    - Pointer to hold the top most value
*  bottom - Pointer to hold the bottom most value
*********************************************************/
void startsGetMaxs(starts *value, int *leftPos, int *rightPos, int *topPos, int *bottomPos);

/*********************************************************
*NAME:          startsMoveAll
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Repositions the starts by moveX, moveY
*
*ARGUMENTS:
*  value  - Pointer to the pills structure
*  moveX  - The X move amount 
*  moveY  - The Y move amount 
*********************************************************/
void startsMoveAll(starts *value, int moveX, int moveY);

void startsSetStartCompressData(starts *value, BYTE *buff, int dataLen);
/* Clamps every start field a map can supply. See basesValidate. */
void startsValidate(starts *value);

/* Takes off the map every start in the mined border, unless no start is
 * inside it. Returns how many were taken off. See starts.c. */
BYTE startsRemoveBorderStarts(starts *value);


#endif /* STARTS_H */


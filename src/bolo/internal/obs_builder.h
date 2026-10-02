/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Observation Builder
 *Filename:      obs_builder.h
 *Purpose:
 *  Builds WinBoloObs (V3) from BrainInfo for in-game ML
 *  brain inference.
 *
 *  obsBuildMultiView is the one way in: the tank's own view
 *  rect, plus the rect around each pillbox it or an ally
 *  owns, gathered into one observation.
 *********************************************************/

#ifndef OBS_BUILDER_H
#define OBS_BUILDER_H

#include "brain.h"
#include "../../gym/winbolo_gym.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/*********************************************************
 *NAME:          obsBuildMultiView
 *PURPOSE:
 *  Builds a V3 WinBoloObs with multi-view entity gathering.
 *  Uses the tank-view BrainInfo for terrain, scalars, events,
 *  and initial entity list, then gathers additional dynamic
 *  objects (shells, tanks, LGMs) from owned alive pillbox
 *  view rects (15x15 each) and merges them into the entity
 *  list with deduplication.
 *
 *  This brings obs_builder to parity with the gym's
 *  multi-view gathering (gymBuildObs in winbolo_gym.c).
 *
 *  The entries that normalise against what full means divide
 *  by written-out numbers describing the classic game, so
 *  this refuses to build on a sim whose rules are not the
 *  classic ones: false, with obs left zeroed and a line in
 *  the log naming the first rule that differs. A caller that
 *  gets false has no observation and should not act on one.
 *  See the scale note at the top of obs_builder.c.
 *
 *ARGUMENTS:
 *  cs     - ClientSim pointer (for rect-gathering functions)
 *  tankBi - Populated tank-view BrainInfo
 *  obs    - Output observation (zeroed and filled)
 *
 *RETURNS:
 *  true when the observation was built, false when the sim's
 *  rules are not classic and there was nothing to build on
 *********************************************************/
bool obsBuildMultiView(struct ClientSim *cs, const BrainInfo *tankBi, WinBoloObs *obs);

/*********************************************************
 *NAME:          obsEventIsRead
 *PURPOSE:
 *  Whether obsBuildEventsFrom reads an event type. The gym
 *  copies only these out of the server's event queue, so the
 *  translator and the gym's filter cannot disagree about which
 *  events matter.
 *
 *  Every event type is named in the switch behind this, read
 *  or not, and the unit test walks every type up to EVENT_LAST
 *  and fails on one the switch does not name. A new event type
 *  therefore has to be given an answer here before the tests
 *  pass again.
 *
 *ARGUMENTS:
 *  type - An EVENT_* value
 *
 *RETURNS:
 *  OBS_EVENT_READ, OBS_EVENT_IGNORED, or OBS_EVENT_UNKNOWN
 *  for a type the switch does not name
 *********************************************************/
typedef enum {
    OBS_EVENT_UNKNOWN = 0,
    OBS_EVENT_READ,
    OBS_EVENT_IGNORED
} ObsEventUse;

ObsEventUse obsEventIsRead(uint8_t type);

/*********************************************************
 *NAME:          obsBuildEventsFrom
 *PURPOSE:
 *  Turns a list of game events into the observation's
 *  discrete events, positional sounds and assistant message.
 *  The gym and the in-game ML brain both build through this,
 *  so a trained model hears the same thing in a game that it
 *  heard in training.
 *
 *  Sounds are built only from events a client receives
 *  (never a gameEventIsLocal one), because the in-game brain
 *  reads a client's events and the gym must not hear what
 *  the game will not tell it. Discrete events feed the gym's
 *  rewards, not the model, and may use server-only events.
 *
 *  Hits come from EVENT_TANK_HIT, which is server-only. A
 *  list taken from a client has none, so for one of those
 *  the agent's own hit sound stands in for a hit received,
 *  and a hit dealt is not known at all.
 *
 *  Appends to obs; the caller zeroes it first.
 *
 *ARGUMENTS:
 *  events     - The events to read
 *  count      - How many
 *  fromServer - True for the server's own queue (the gym),
 *               false for a list a client received
 *  selfPlayer - The observing player's slot
 *  allies     - The observing player's allies bitmap
 *  tankTx     - The observing tank's map square X
 *  tankTy     - The observing tank's map square Y
 *  obs        - Output observation
 *********************************************************/
void obsBuildEventsFrom(const GameEvent *events, int count, bool fromServer,
                        BYTE selfPlayer, PlayerBitMap allies, int tankTx,
                        int tankTy, WinBoloObs *obs);

#ifdef __cplusplus
}
#endif

#endif /* OBS_BUILDER_H */

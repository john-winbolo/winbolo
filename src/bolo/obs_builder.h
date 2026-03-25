/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *Name:          Observation Builder
 *Filename:      obs_builder.h
 *Purpose:
 *  Builds WinBoloObs (V3) from BrainInfo for in-game ML
 *  brain inference.
 *
 *  obsBuildFromBrainInfo: single-view (tank view rect)
 *  obsBuildMultiView:     multi-view (tank + owned pills)
 *********************************************************/

#ifndef OBS_BUILDER_H
#define OBS_BUILDER_H

#include "brain.h"
#include "../gym/winbolo_gym.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/*********************************************************
 *NAME:          obsBuildFromBrainInfo
 *PURPOSE:
 *  Builds a complete V3 WinBoloObs from BrainInfo alone.
 *  Entity list is built from BrainInfo visible objects.
 *  Single-view (tank view rect) only.
 *
 *ARGUMENTS:
 *  bi  - Populated BrainInfo from screenMakeBrainInfoCS
 *  obs - Output observation (zeroed and filled)
 *********************************************************/
void obsBuildFromBrainInfo(const BrainInfo *bi, WinBoloObs *obs);

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
 *ARGUMENTS:
 *  cs     - ClientSim pointer (for rect-gathering functions)
 *  tankBi - Populated tank-view BrainInfo
 *  obs    - Output observation (zeroed and filled)
 *********************************************************/
void obsBuildMultiView(struct ClientSim *cs, const BrainInfo *tankBi, WinBoloObs *obs);

#ifdef __cplusplus
}
#endif

#endif /* OBS_BUILDER_H */

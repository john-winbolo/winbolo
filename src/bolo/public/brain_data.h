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
 *Name:          Brain Data
 *Filename:      brain_data.h
 *Purpose:
 *  Public API for brain_data.c — builds the data the Lua
 *  brain reads each tick (terrain view buffer, BrainInfo
 *  struct, brain-object list) and reads decisions back.
 *********************************************************/

#ifndef BRAIN_DATA_H
#define BRAIN_DATA_H

#include "global.h"
#include "brain.h"
#include "client_enums.h"   /* aiType */

struct ClientSim;

void brainDataMakeViewData(struct ClientSim *cs, BYTE *buff, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos);
void brainDataMakeInfo(struct ClientSim *cs, BrainInfo *value, bool first, aiType aiMode);
void brainDataExtractInfo(struct ClientSim *cs, BrainInfo *value);
void brainDataAddObject(struct ClientSim *cs, unsigned short object, WORLD wx, WORLD wy, unsigned short idNum, BYTE dir, BYTE info, BYTE speed);

#endif /* BRAIN_DATA_H */

/*
 * $Id$
 *
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
*Name:          Labels
*Filename:      label.h
*Author:        John Morrison
*Creation Date:  2/2/99
*Last Modified:  2/2/99
*Purpose:
*  Responsable for message labels (short/long etc).
*********************************************************/

#ifndef LABELS_H
#define LABELS_H

#include "global.h"

struct ClientSim;

/* The @ Symbol */
#define LABEL_AT_SYMBOL "@\0"

/* Prototypes */

void labelMakeMessage(struct ClientSim *cs, char *res, char *name, char *loc);
void labelMakeTankLabel(struct ClientSim *cs, char *res, char *name, char *loc, bool isOwn);

#endif /* LABELS_H */

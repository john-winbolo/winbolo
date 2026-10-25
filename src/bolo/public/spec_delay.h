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
 *Name:          Spectator Delay
 *Filename:      spec_delay.h
 *Author:        John Morrison
 *Purpose:
 *  The spectator delay a desktop host applies to the game
 *  it hosts, in seconds. The dedicated server's -specdelay
 *  default (90) is separate and lives in servermain.c.
 *********************************************************/

#ifndef SPEC_DELAY_H
#define SPEC_DELAY_H

/* The delay when the [HOSTING] "Spectator Delay" preference does not say
 * otherwise. */
#define HOSTING_SPEC_DELAY_DEFAULT_SECS 30
/* The longest delay the Hosting tab offers. The spectator ring keeps the
 * whole delay in memory, so the desktop host stops at ten minutes. */
#define HOSTING_SPEC_DELAY_MAX_SECS     600

#endif /* SPEC_DELAY_H */

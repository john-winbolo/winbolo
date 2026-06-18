/*
 * $Id$
 *
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
*Name:          Sound
*Filename:      sound.h
*Author:        John Morrison
*Creation Date: 26/12/98
*Last Modified: 24/02/02
*Purpose:
*  System Specific Sound Playing routines
*  (Uses SDL3)
*********************************************************/

#ifndef SOUND_H
#define SOUND_H

#include "global.h"
#include "client_enums.h"  /* sndEffects */

/*********************************************************
*NAME:          soundSetup
*AUTHOR:        John Morrison
*CREATION DATE: 26/10/98
*LAST MODIFIED: 26/10/98
*PURPOSE:
*  Sets up sound systems, SDL3 audio structures etc.
*  Returns whether the operation was successful or not
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool soundSetup(void);

/*********************************************************
*NAME:          soundCleanup
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 26/12/98
*PURPOSE:
*  Destroys and cleans up sound systems, SDL3 audio
*  structures etc.
*
*ARGUMENTS:
*
*********************************************************/
void soundCleanup(void);

/*********************************************************
*NAME:          soundPlayEffect
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
*  Plays the correct Sound file
*
*ARGUMENTS:
*  value       - The sound file number to play
*********************************************************/
void soundPlayEffect(sndEffects value);

/*********************************************************
*NAME:          soundKeepalive
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 29/12/98
*PURPOSE:
*  Some AV receivers go to sleep if we don't output a
*  constant data stream, especially with Spatial Audio.
*
*ARGUMENTS:
*  value - TRUE to turn on FALSE to turn off.
*********************************************************/
void soundKeepalive(bool value);

/*********************************************************
*NAME:          soundIsPlayable
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Returns whether the sound system is enabled or not. By
*  enabled I mean an error hasn't stopped us from starting
*  it
*
*ARGUMENTS:
*
*********************************************************/
bool soundIsPlayable(void);

/*********************************************************
*NAME:          soundSetMuted
*PURPOSE:
*  Pauses or resumes the main audio stream.  Used to
*  silence sound effects when the window loses focus
*  and "Background Sound" is disabled.
*
*ARGUMENTS:
*  mute - TRUE to pause, FALSE to resume
*********************************************************/
void soundSetMuted(bool mute);

/*********************************************************
*NAME:          soundIsMuted
*PURPOSE:
*  Returns the current logical mute state, so a pause path
*  can save it on entry and restore it on exit rather than
*  unconditionally unmuting.
*********************************************************/
bool soundIsMuted(void);

/*********************************************************
*NAME:          soundSetVolume
*PURPOSE:
*  Sets the master output gain on the audio stream.
*
*ARGUMENTS:
*  pct - volume percentage in [0, 100]
*********************************************************/
void soundSetVolume(int pct);

/*********************************************************
*NAME:          soundSetReturningToLobby
*PURPOSE:
*  Saves and zeros the output gain while the game-over →
*  lobby transition is held, then restores the previous gain
*  on the way out. Idempotent: repeat calls with the same
*  state are no-ops, so the user's chosen volume (including
*  a muted 0) is preserved regardless of how many phase
*  events fan out.
*
*ARGUMENTS:
*  active - TRUE to snapshot+silence, FALSE to restore
*********************************************************/
void soundSetReturningToLobby(bool active);

#endif /* SOUND_H */

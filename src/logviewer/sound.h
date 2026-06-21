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
*Name:          Sound
*Filename:      sound.h
*Author:        John Morrison
*Creation Date: 05/05/01
*Last Modified: 05/05/01
*Purpose:
*  System Specific Sound Playing routines 
*  (Uses SDL3)
*********************************************************/

#ifndef _SOUND_H 
#define _SOUND_H 

#include "lv_global.h"
#include "backend.h"

/*********************************************************
*NAME:          lv_soundSetup
*AUTHOR:        John Morrison
*CREATION DATE: 26/10/98
*LAST MODIFIED: 26/10/98
*PURPOSE:
*  Sets up sound systems, SDL3 audio structures etc.
*  Returns whether the operation was successful or not
*
*ARGUMENTS:
* appInst - Handle to the application (Required to 
*           load resources from BoloSounds.bsd DLL)
* appWnd  - Main Window Handle (Unused, kept for API compatibility)
*********************************************************/
bool lv_soundSetup(void);

/*********************************************************
*NAME:          lv_soundCleanup
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
void lv_soundCleanup(void);

/*********************************************************
*NAME:          lv_soundPlayEffect
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
*  Plays the correct Sound file
*
*ARGUMENTS:
*  value       - The sound file number to play
*********************************************************/
void lv_soundPlayEffect(sndEffects value);

/*********************************************************
*NAME:          lv_soundKeepalive
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 29/12/98
*PURPOSE:
*  Some AV receivers go to sleep if we don't play a
*  constant data stream.
*
*ARGUMENTS:
*  value - TRUE to turn on FALSE to turn off.
*********************************************************/
void lv_soundKeepalive(bool value);

/*********************************************************
*NAME:          lv_soundIsPlayable
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
bool lv_soundIsPlayable(void);

/*********************************************************
*NAME:          lv_soundSetVolume
*PURPOSE:
*  Sets the master output gain on the audio stream.
*
*ARGUMENTS:
*  pct - volume percentage in [0, 100]
*********************************************************/
void lv_soundSetVolume(int pct);

#endif /* _SOUND_H  */

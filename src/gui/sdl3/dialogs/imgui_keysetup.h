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
 * Name:          imgui_keysetup.h
 * Purpose:       Standalone key setup dialog shown from
 *                the pre-game settings screen.
 *********************************************************/

#ifndef IMGUI_KEYSETUP_H
#define IMGUI_KEYSETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the key setup dialog as a blocking modal loop.
 * Returns 1 if keys were saved, 0 if cancelled. */
int imguiKeySetupShow(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_KEYSETUP_H */

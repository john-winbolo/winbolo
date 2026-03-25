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
 * Name:          imgui_setname.h
 * Purpose:       ImGui Set Player Name dialog.
 *********************************************************/

#ifndef IMGUI_SETNAME_H
#define IMGUI_SETNAME_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the ImGui set-name dialog as a blocking modal loop.
 * If inGame is true, reads/writes the name via screen*PlayerName;
 * otherwise uses gameFront*PlayerName. */
struct ClientSim;
void imguiSetNameShow(struct ClientSim *cs, bool inGame);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SETNAME_H */

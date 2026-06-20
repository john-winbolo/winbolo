/*
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
 * Name:          imgui_udpsetup.h
 * Purpose:       ImGui UDP (Internet) Setup dialog.
 *********************************************************/

#ifndef IMGUI_UDPSETUP_H
#define IMGUI_UDPSETUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the ImGui UDP setup dialog as a blocking modal loop.
 * Sets dlgState via gameFrontSetDlgState before returning.
 * Returns 1 if an action was taken, 0 if cancelled. */
int imguiUdpSetupShow(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_UDPSETUP_H */

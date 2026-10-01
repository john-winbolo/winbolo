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
 * Name:          imgui_welcome.h
 * Purpose:       ImGui Welcome/Opening dialog.
 *********************************************************/

#ifndef IMGUI_WELCOME_H
#define IMGUI_WELCOME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the ImGui welcome dialog as a blocking modal loop.
 * Returns the selected mode as an int matching openingStates enum values,
 * or -1 if the user cancelled/closed the dialog. */
int imguiWelcomeShow(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_WELCOME_H */

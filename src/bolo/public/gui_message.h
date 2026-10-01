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
*Name:          gui_message
*Filename:      gui_message.h
*Purpose:
*  Platform-independent message box abstraction.
*  The GUI layer registers a handler; backend code
*  (e.g. network.c) calls guiMessageShow() without
*  knowing whether wx, SDL, or Win32 provides the dialog.
*********************************************************/

#ifndef GUI_MESSAGE_H
#define GUI_MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DIALOG_BOX_TITLE
#define DIALOG_BOX_TITLE "WinBolo"
#endif

typedef void (*gui_message_fn)(const char *message, const char *title);

void guiMessageSetHandler(gui_message_fn handler);
void guiMessageShow(const char *message, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* GUI_MESSAGE_H */

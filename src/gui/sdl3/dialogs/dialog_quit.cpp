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
 *Name:          dialog_quit
 *Filename:      dialog_quit.cpp
 *Purpose:
 *  Where a quit seen inside a dialog goes.
 *
 *  Every screen runs its own event loop, so a quit has to
 *  be recognised in each of them (dialogHandleQuitEvent in
 *  imgui_dialog_utils.h) and then handed to whoever owns
 *  the application.  These dialogs are linked into the
 *  standalone Log Viewer and Map Editor as well as into
 *  WinBolo, and those have no such owner, so the host
 *  registers what a quit means rather than the dialogs
 *  calling into the game directly.
 *
 *  With no handler registered a quit just closes the
 *  dialog, which is what every screen did before.
 *********************************************************/

extern "C" {

static void (*s_quitHandler)(void) = nullptr;

void dialogSetQuitHandler(void (*handler)(void)) {
    s_quitHandler = handler;
}

void dialogRequestQuit(void) {
    if (s_quitHandler != nullptr) s_quitHandler();
}

} /* extern "C" */

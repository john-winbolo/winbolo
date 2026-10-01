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
 *Name:          dialog_quit
 *Filename:      dialog_quit.cpp
 *Purpose:
 *  Where a quit seen inside a dialog goes.
 *
 *  Every screen runs its own event loop, so a quit has to
 *  be recognised in each of them (dialogHandleQuitEvent in
 *  imgui_dialog_utils.h, which wraps the decision below)
 *  and then handed to whoever owns the application.  These
 *  dialogs are linked into the standalone Log Viewer and
 *  Map Editor as well as into WinBolo, and those have no
 *  such owner, so the host registers what a quit means
 *  rather than the dialogs calling into the game directly.
 *
 *  With no handler registered a quit just closes the
 *  dialog, which is what every screen did before.
 *
 *  The decision itself is here, and not inline beside the
 *  dialogs, so it can be tested: it opens no window, holds
 *  no ImGui context and calls nothing back into SDL — the
 *  "leaf a test can call with no display attached" rule in
 *  docs/ARCHITECTURE.md.
 *********************************************************/

#include "dialog_quit.h"

extern "C" {

DialogQuitAction dialogQuitClassify(const SDL_Event *ev,
                                    SDL_WindowID dialogWindowID) {
    if (ev == nullptr) return DIALOG_QUIT_NONE;

    /* Cmd+Q and the like.  Not addressed to any one window, so there is
     * nothing to match against — whichever dialog is up sees it. */
    if (ev->type == SDL_EVENT_QUIT) return DIALOG_QUIT_APPLICATION;

    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        /* A dialog with no window of its own can own no close request;
         * SDL never hands out 0 as a window id. */
        if (dialogWindowID == 0) return DIALOG_QUIT_NONE;
        if (ev->window.windowID != dialogWindowID) return DIALOG_QUIT_NONE;
        /* Alt+F4 and the close box end the application.  The B button's
         * cancel reaches us as the close request
         * dialogHandleGamepadCancelEvent forged, marked as its own, and
         * only closes the dialog. */
        return (ev->window.data1 == DIALOG_CLOSE_IS_GAMEPAD_CANCEL)
                   ? DIALOG_QUIT_CLOSE_DIALOG
                   : DIALOG_QUIT_APPLICATION;
    }

    return DIALOG_QUIT_NONE;
}

static void (*s_quitHandler)(void) = nullptr;

void dialogSetQuitHandler(void (*handler)(void)) {
    s_quitHandler = handler;
}

void dialogRequestQuit(void) {
    if (s_quitHandler != nullptr) s_quitHandler();
}

} /* extern "C" */

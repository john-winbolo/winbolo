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
 *Filename:      dialog_quit.h
 *Purpose:       What a quit seen inside a dialog means, and
 *               where it goes.
 *********************************************************/

#ifndef DIALOG_QUIT_H
#define DIALOG_QUIT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A close request the B button forged carries this in window.data1, so
 * dialogQuitClassify can tell it apart from a real one.  Both close the
 * dialog; only the real one ends the application.
 *
 * The marker rides in the event rather than in a flag beside it because
 * the answer has to travel with the event: a dialog loop hands the same
 * SDL_Event to several handlers in turn, and one of them may consume it
 * before the quit check is reached.
 *
 * SDL leaves data1 at zero on the close requests it sends itself — every
 * backend sends SDL_SendWindowEvent(window, ..._CLOSE_REQUESTED, 0, 0).
 * The value is a sentinel rather than a small integer so a close request
 * from anywhere else cannot resemble one of ours by accident. */
#define DIALOG_CLOSE_IS_GAMEPAD_CANCEL 0x42434E4C /* 'BCNL' */

typedef enum {
    /* Not a quit-shaped event, or a close request for someone else's
     * window.  The dialog carries on polling. */
    DIALOG_QUIT_NONE = 0,
    /* Close this dialog, the application carries on: the B button's
     * cancel, which reaches the loop as a forged close request. */
    DIALOG_QUIT_CLOSE_DIALOG,
    /* Close this dialog and end the application: Cmd+Q, Alt+F4, the
     * window's close box. */
    DIALOG_QUIT_APPLICATION
} DialogQuitAction;

/*********************************************************
*NAME:          dialogQuitClassify
*PURPOSE:
* What one polled event means to the dialog that is up.
* The whole decision, and it makes no SDL calls of its own,
* so it can be driven with no window attached.
*
*ARGUMENTS:
* ev             - the event polled, may be NULL
* dialogWindowID - the dialog's window, 0 if it has none
*********************************************************/
DialogQuitAction dialogQuitClassify(const SDL_Event *ev,
                                    SDL_WindowID dialogWindowID);

/*********************************************************
*NAME:          dialogSetQuitHandler
*PURPOSE:
* Tell the shared dialogs where a quit goes.  They are
* linked into the standalone Log Viewer and Map Editor too,
* which have no application loop of their own, so WinBolo
* registers windowSetQuitting here at startup rather than
* the dialogs calling it directly.  With nothing registered
* a quit just closes the dialog, which is what every screen
* did before.
*
*ARGUMENTS:
* handler - what a quit seen in a dialog should do
*********************************************************/
void dialogSetQuitHandler(void (*handler)(void));

/*********************************************************
*NAME:          dialogRequestQuit
*PURPOSE:
* Hand a quit to the registered handler.  Does nothing when
* there is none.
*
*ARGUMENTS:
*********************************************************/
void dialogRequestQuit(void);

#ifdef __cplusplus
}
#endif

#endif /* DIALOG_QUIT_H */

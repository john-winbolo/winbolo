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

/*
 * What a quit seen inside a dialog means (dialogQuitClassify) and where it
 * goes (dialogSetQuitHandler / dialogRequestQuit).
 *
 * Every screen runs its own SDL_PollEvent loop, so the same decision is
 * made eleven times over and there is no compiler check that a new dialog
 * makes it at all.  The one that matters most has no visible symptom until
 * a player hits it: a gamepad's B button reaches a dialog as a *forged*
 * close request, and telling it apart from a real one is the difference
 * between cancelling a dialog and ending the application under someone who
 * only wanted to back out of it.
 *
 * Driven with no window, no renderer and no ImGui context — the events are
 * built by hand.
 */

#include <SDL3/SDL.h>

#include "test_harness.h"
#include "dialog_quit.h"

/* An arbitrary stand-in for "the dialog that is up". SDL never hands out 0
 * as a window id, which is what dialogQuitClassify keys "no window" on, so
 * any non-zero value will do. */
#define UT_DIALOG_WINDOW ((SDL_WindowID)7)
#define UT_OTHER_WINDOW  ((SDL_WindowID)8)

static int s_quitCalls = 0;

static void utCountQuit(void) { s_quitCalls++; }

/* A close request as SDL itself sends one: every backend calls
 * SDL_SendWindowEvent(window, ..._CLOSE_REQUESTED, 0, 0). */
static SDL_Event utCloseRequest(SDL_WindowID id) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    ev.window.windowID = id;
    return ev;
}

/* The same event as dialogHandleGamepadCancelEvent forges it for B. */
static SDL_Event utGamepadCancel(SDL_WindowID id) {
    SDL_Event ev = utCloseRequest(id);
    ev.window.data1 = DIALOG_CLOSE_IS_GAMEPAD_CANCEL;
    return ev;
}

int run_dialog_quit(void) {
    SDL_Event ev;

    /* ---- Cmd+Q ends the application, whichever dialog is up ---- */
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) ==
                      DIALOG_QUIT_APPLICATION,
                  "SDL_EVENT_QUIT must end the application");
    /* It is addressed to no window, so a dialog without one still sees it. */
    UT_ASSERT_MSG(dialogQuitClassify(&ev, 0) == DIALOG_QUIT_APPLICATION,
                  "SDL_EVENT_QUIT must not depend on owning a window");

    /* ---- Alt+F4 / the close box on our own window ends it too ---- */
    ev = utCloseRequest(UT_DIALOG_WINDOW);
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) ==
                      DIALOG_QUIT_APPLICATION,
                  "a real close request must end the application");

    /* ---- B cancels: closes the dialog, application carries on ---- */
    ev = utGamepadCancel(UT_DIALOG_WINDOW);
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) ==
                      DIALOG_QUIT_CLOSE_DIALOG,
                  "the B button's forged close must not end the application");

    /* ---- Somebody else's window is not ours to close ---- */
    ev = utCloseRequest(UT_OTHER_WINDOW);
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) == DIALOG_QUIT_NONE,
                  "a close request for another window must be ignored");

    /* A dialog with no window of its own owns no close request at all —
     * otherwise the embedded Map Editor and Log Viewer, which draw into
     * WinBolo's window, would answer for it. */
    ev = utCloseRequest(UT_DIALOG_WINDOW);
    UT_ASSERT_MSG(dialogQuitClassify(&ev, 0) == DIALOG_QUIT_NONE,
                  "a windowless dialog must claim no close request");

    /* ---- Anything else is not a quit ---- */
    SDL_zero(ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) == DIALOG_QUIT_NONE,
                  "a key press is not a quit");
    SDL_zero(ev);
    ev.type = SDL_EVENT_WINDOW_RESIZED;
    ev.window.windowID = UT_DIALOG_WINDOW;
    /* data1/data2 carry the new size here, so a window event that happens to
     * put something in data1 must not read as a cancel — or as a quit. */
    ev.window.data1 = DIALOG_CLOSE_IS_GAMEPAD_CANCEL;
    UT_ASSERT_MSG(dialogQuitClassify(&ev, UT_DIALOG_WINDOW) == DIALOG_QUIT_NONE,
                  "a resize is not a quit whatever data1 holds");

    UT_ASSERT_MSG(dialogQuitClassify(NULL, UT_DIALOG_WINDOW) == DIALOG_QUIT_NONE,
                  "no event is no quit");

    /* ---- Where the quit goes ---- */

    /* With nothing registered a quit just closes the dialog. That is the
     * standalone Log Viewer and Map Editor, which link these dialogs but
     * have no application loop to end. Reaching it must not crash. */
    dialogSetQuitHandler(NULL);
    dialogRequestQuit();

    s_quitCalls = 0;
    dialogSetQuitHandler(utCountQuit);
    dialogRequestQuit();
    UT_ASSERT_MSG(s_quitCalls == 1, "registered handler ran %d times, wanted 1",
                  s_quitCalls);

    dialogRequestQuit();
    UT_ASSERT_MSG(s_quitCalls == 2, "handler ran %d times over two quits",
                  s_quitCalls);

    /* Leave no handler behind for whatever test runs next in this process. */
    dialogSetQuitHandler(NULL);
    return 0;
}

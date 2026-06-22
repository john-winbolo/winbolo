/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_prompt.h
 * Purpose:       Controller-detected prompt (Phase 8.1).
 *                One-shot modal asking the player whether
 *                to switch to Controller Mode when a
 *                gamepad first connects on a non-Deck
 *                desktop.  Yes flips the pref to ON; No
 *                dismisses for the session; Don't ask
 *                again clears the prompt-on-connect flag.
 *********************************************************/

#ifndef IMGUI_CONTROLLER_PROMPT_H
#define IMGUI_CONTROLLER_PROMPT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void controllerPromptOpen(void);                 /* request open */
void controllerPromptRender(void);               /* call from render path */
bool controllerPromptIsOpen(void);

/* Poll the gamepad-connected rising edge and open the prompt when a pad first
   connects on a non-Deck, non-tablet desktop with controller mode off and the
   prompt not opted out.  Single-sourced edge state, shared by the in-game render
   path and the menu loops. */
void controllerPromptPollConnectEdge(void);

/* Detect + render both controller dialogs (connect prompt, disconnect dialog)
   for a menu/standalone context.  No solo-game pause (no game is running). */
void controllerDialogsRenderMenu(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTROLLER_PROMPT_H */

/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_controller_prompt.h
 * Purpose:       Per-loop hook that raises the shared
 *                controller dialogs (the "Controller
 *                Disconnected" alert) over the running
 *                menu/dialog loop.
 *********************************************************/

#ifndef IMGUI_CONTROLLER_PROMPT_H
#define IMGUI_CONTROLLER_PROMPT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Detect + render the controller dialogs for a menu/standalone context.
   No solo-game pause (no game is running). */
void controllerDialogsRenderMenu(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONTROLLER_PROMPT_H */

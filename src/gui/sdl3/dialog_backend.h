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
 * Name:          dialog_backend.h
 * Purpose:       Dispatch table for pre-game dialogs
 *                (ImGui implementation).
 *********************************************************/

#ifndef DIALOG_BACKEND_H
#define DIALOG_BACKEND_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

typedef struct {
  int  (*welcomeShow)(void);
  int  (*udpSetupShow)(void);
  int  (*gameBrowserShow)(const char *title, int useTracker);
  void (*setNameShow)(struct ClientSim *cs, bool inGame);
  int  (*trackerSetupShow)(void);
  void (*messageBox)(const char *msg, const char *title);
  int  (*lobbyShow)(struct ClientSim *cs);
  void (*settingsShow)(void);
} DialogBackend;

/* Initialise the dialog backend. */
void dialogBackendInit(void);

/* Return the active dialog backend dispatch table. */
const DialogBackend *dialogBackendGet(void);

#ifdef __cplusplus
}
#endif

#endif /* DIALOG_BACKEND_H */

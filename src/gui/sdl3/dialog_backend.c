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
 * Name:          dialog_backend.c
 * Purpose:       Dispatch table for ImGui pre-game dialog
 *                backend.
 *********************************************************/

#include "dialog_backend.h"

#include "dialogs/imgui_welcome.h"
#include "dialogs/imgui_udpsetup.h"
#include "dialogs/imgui_gamebrowser.h"
#include "dialogs/imgui_setname.h"
#include "dialogs/imgui_trackersetup.h"
#include "dialogs/imgui_messagebox.h"
#include "dialogs/imgui_lobby.h"
#include "dialogs/imgui_settings.h"

static const DialogBackend imguiBackend = {
  imguiWelcomeShow,
  imguiUdpSetupShow,
  imguiGameBrowserShow,
  imguiSetNameShow,
  imguiTrackerSetupShow,
  imguiMessageBox,
  imguiLobbyShow,
  imguiSettingsShow,
};

static const DialogBackend *activeBackend = &imguiBackend;

void dialogBackendInit(void) {
  activeBackend = &imguiBackend;
}

const DialogBackend *dialogBackendGet(void) {
  return activeBackend;
}

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
 * Name:          imgui_lobby.h
 * Purpose:       ImGui Lobby dialog.
 *********************************************************/

#ifndef IMGUI_LOBBY_H
#define IMGUI_LOBBY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the ImGui lobby dialog as a blocking modal loop.
 * Returns 1 if the game started, 0 if the player chose to leave. */
struct ClientSim;
int imguiLobbyShow(struct ClientSim *cs);

/* Per-frame lobby renderer for hosts that drive a single shared ImGui
 * context from a non-blocking main loop (the WASM/emscripten client).
 *
 * Builds the lobby UI into the CURRENTLY-ACTIVE ImGui frame: the caller
 * must have already pumped SDL events and called ImGui::NewFrame(), and
 * is responsible for ImGui::Render()/present afterwards. This function
 * does NOT create/destroy an ImGui context, init SDL backends, pump
 * events, tick the transport, or resize/retitle the window.
 *
 * All per-frame UI state (chat input, scroll/tab selection, map-preview
 * texture, countdown tracker, ...) is kept in file-static storage that
 * persists across calls and is (re)initialised on the first call after a
 * fresh lobby is entered. Call imguiLobbyFrameReset() when leaving the
 * lobby to release that state (the blocking imguiLobbyShow does this on
 * teardown automatically).
 *
 * Returns LOBBY_FRAME_LEFT once the player confirms leaving (the host
 * should then disconnect/quit), otherwise LOBBY_FRAME_CONTINUE. Game
 * start is NOT signalled here: the host detects it from the sim leaving
 * the lobby state (clientSimIsInLobby flips false). */
typedef enum {
    LOBBY_FRAME_CONTINUE = 0,
    LOBBY_FRAME_LEFT     = 1
} LobbyFrameStatus;

LobbyFrameStatus imguiLobbyRenderFrame(struct ClientSim *cs);

/* Release per-frame lobby state (map-preview texture, popup buffers, and
 * every transient visibility/pending flag). Safe to call when not in a
 * lobby. */
void imguiLobbyFrameReset(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_LOBBY_H */

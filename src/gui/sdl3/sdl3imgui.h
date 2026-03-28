/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          SDL3 ImGui Menu Bar
*Filename:      sdl3imgui.h
*Purpose:
*  C-compatible wrapper around the ImGui menu bar and
*  dialog panels rendered into the SDL3 window.  C++
*  implementation lives in sdl3imgui.cpp.
*
*  Include AFTER <SDL3/SDL.h> (sdl3draw.c already does this).
*********************************************************/

#ifndef SDL3IMGUI_H
#define SDL3IMGUI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

struct ClientSim;

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
*NAME:          sdl3ImguiSetup
*PURPOSE:
*  Initialise ImGui, attach the SDL3 + SDLRenderer3
*  backends to the given window and renderer.
*  Must be called after SDL_CreateWindow /
*  SDL_CreateRenderer.  Returns true on success.
*********************************************************/
bool sdl3ImguiSetup(SDL_Window *window, SDL_Renderer *renderer);

/*********************************************************
*NAME:          sdl3ImguiPumpAndRender
*PURPOSE:
*  1. Drains the SDL event queue and forwards each event
*     to ImGui_ImplSDL3_ProcessEvent so mouse / keyboard
*     input reaches the menu bar.
*  2. Builds and renders a single ImGui frame containing
*     only the main menu bar (mirroring the Win32 menu).
*  Call this every frame immediately before
*  SDL_RenderPresent.
*********************************************************/
/*********************************************************
*NAME:          sdl3ImguiProcessEvents
*PURPOSE:
*  Drains the SDL event queue and forwards each event to
*  ImGui so that mouse/keyboard input reaches the menu bar.
*  MUST be called from the main thread only (SDL asserts
*  this). Call from the main event loop, NOT from a timer
*  callback thread.
*********************************************************/
void sdl3ImguiProcessEvents(struct ClientSim *cs);
void sdl3ImguiResetFrameState(void);

void sdl3ImguiPumpAndRender(struct ClientSim *cs);
void sdl3ImguiClearNavFocus(void);
void sdl3ImguiForwardEvent(const void *event);

/*********************************************************
*NAME:          sdl3ImguiSetExtraRenderCallback
*PURPOSE:
*  Register a callback that is invoked during each ImGui
*  frame, after the menu bar and info panels but before
*  EndFrame/Render.  Used by Android/iOS to inject the
*  players panel into the ImGui frame.  Pass NULL to clear.
*********************************************************/
typedef void (*sdl3ImguiExtraRenderFn)(struct ClientSim *cs);
void sdl3ImguiSetExtraRenderCallback(sdl3ImguiExtraRenderFn fn);

/*********************************************************
*NAME:          sdl3ImguiCleanup
*PURPOSE:
*  Shutdown ImGui backends and destroy the ImGui context.
*********************************************************/
void sdl3ImguiCleanup(void);

/*********************************************************
*NAME:          sdl3ImguiShowSysInfo / ShowNetInfo / ShowGameInfo
*PURPOSE:
*  Open (true) or close (false) the corresponding info
*  window.  May be called from menu command handlers
*  instead of creating separate dialog windows.
*********************************************************/
void sdl3ImguiShowSysInfo(bool open);
void sdl3ImguiShowNetInfo(bool open);
void sdl3ImguiShowGameInfo(bool open);
void sdl3ImguiShowSendMsg(bool open);
void sdl3ImguiShowPlayersPanel(bool open);

/*********************************************************
*NAME:          sdl3ImguiShowAllianceRequest
*PURPOSE:
*  Open the "Alliance Request" modal.  playerName is the
*  requester; playerNum is passed to netAllianceAccept()
*  if the user clicks Accept.  Safe to call from the game
*  logic thread (state is set; modal opens next frame).
*********************************************************/
void sdl3ImguiShowAllianceRequest(const char *playerName,
                                  unsigned char playerNum);

/*********************************************************
*NAME:          sdl3ImguiShowPassword
*PURPOSE:
*  Open the password-entry modal for joining a protected
*  game.  On OK calls gameFrontSetGameOptions with the
*  entered password.
*********************************************************/
void sdl3ImguiShowPassword(void);

/*********************************************************
*NAME:          sdl3ImguiShowKeySetup
*PURPOSE:
*  Open the Key Setup modal dialog allowing the user to
*  rebind all game keys.  Keyboard and mouse events are
*  fully consumed by the modal until it is dismissed.
*********************************************************/
void sdl3ImguiShowKeySetup(void);

/*********************************************************
*NAME:          sdl3ImguiShowSettings
*PURPOSE:
*  Toggle the Settings panel.  Collects display, label,
*  sound, message, and player options into one window.
*  On mobile this is the primary way to access settings
*  since there is no menu bar.
*********************************************************/
void sdl3ImguiShowSettings(void);

/*********************************************************
*NAME:          sdl3ImguiWantsKeyboard
*PURPOSE:
*  Returns true when ImGui has an active text input widget
*  (e.g. the Send Message box).  input.c checks this before
*  polling GetAsyncKeyState() so game keys are suppressed
*  while the user is typing.
*********************************************************/
bool sdl3ImguiWantsKeyboard(void);

/*********************************************************
*NAME:          sdl3ImguiSetPlayer / ClearPlayer / SetPlayerCheckState
*PURPOSE:
*  Update the Players menu state.  Called from the frontend
*  callbacks (frontEndSetPlayer etc.) in winbolo.c so that
*  the ImGui Players menu reflects the current player list.
*  playerNum is 0-based (player01 == 0).
*********************************************************/
void sdl3ImguiSetPlayer(unsigned char playerNum, const char *name, const char *countryCode);
void sdl3ImguiClearPlayer(unsigned char playerNum);
void sdl3ImguiSetPlayerCheckState(unsigned char playerNum, bool isChecked);
void sdl3ImguiUpdatePlayerMeta(unsigned char playerNum, uint16_t ping, bool wbn, bool steam);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_H */

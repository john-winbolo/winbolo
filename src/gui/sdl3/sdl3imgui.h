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

/* UI scale applied to the main ImGui context (font + style) in sdl3ImguiSetup. */
float sdl3ImguiGetUiScale(void);

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
bool sdl3ImguiWantCaptureMouse(void);
bool sdl3ImguiIsDialogOpen(void);

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
void sdl3ImguiTogglePlayersPanel(void);

/* True iff the corresponding info / send-message popout is currently
 * open. On desktop the popout lives in its own native window; on the
 * web / Android / iOS / tablet builds it lives inline in the ImGui
 * frame. The wrappers above hide that split — these accessors return
 * the matching open-state from the same branch. */
bool sdl3ImguiIsSysInfoOpen(void);
bool sdl3ImguiIsNetInfoOpen(void);
bool sdl3ImguiIsGameInfoOpen(void);
bool sdl3ImguiIsSendMsgOpen(void);

/*********************************************************
*NAME:          sdl3ImguiAllianceReqInCooldown
*PURPOSE:
*  Is an alliance-request cooldown currently active? Mirrors
*  the in-window Players menu's gating so the native macOS
*  menu items grey out identically.
*********************************************************/
bool sdl3ImguiAllianceReqInCooldown(void);

/*********************************************************
*NAME:          sdl3ImguiNoteAllianceRequested
*PURPOSE:
*  Mark an alliance request as just-fired. Starts the
*  standard cooldown window; the matching menu items will
*  report inCooldown=true until it elapses.
*********************************************************/
void sdl3ImguiNoteAllianceRequested(void);

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

void sdl3ImguiShowAbout(void);
void sdl3ImguiShowChangeName(void);

/* Thin int-parameter wrappers used by the native macOS menu bar so the
 * .mm file doesn't need to pull in bolo enum / dialog-utils headers. */
void sdl3ImguiSetFrameRate(int rate);
void sdl3ImguiSetZoom(int zoom);
void sdl3ImguiSetMessageLabelLen(struct ClientSim *cs, int len);
void sdl3ImguiSetTankLabelLen(struct ClientSim *cs, int len);

/*********************************************************
*NAME:          sdl3ImguiStopBrain
*PURPOSE:
*  Stop whichever brain (Lua or ONNX) is currently running.
*  No-op if none is running. Called from the macOS native
*  Brains > Manual menu item.
*********************************************************/
void sdl3ImguiStopBrain(void);

/*********************************************************
*NAME:          sdl3ImguiStartBrain
*PURPOSE:
*  Start the brain at index `idx`. Routes to luaBrainStart()
*  for Lua brains and mlBrainStartSingleton() for ONNX brains;
*  also resets the brain-settings descriptor so a re-open of
*  the Settings dialog rebuilds it for the new brain. No-op
*  if idx is out of range or the brain has no path.
*********************************************************/
void sdl3ImguiStartBrain(int idx, struct ClientSim *cs);

/*********************************************************
*NAME:          sdl3ImguiShowBrainSettings
*PURPOSE:
*  Open the brain-settings dialog. Re-fetches the settings
*  descriptor so values reflect the currently-running brain.
*  Called from the macOS native Brains > Settings menu item.
*********************************************************/
void sdl3ImguiShowBrainSettings(void);

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
void sdl3ImguiUpdatePlayerMeta(unsigned char playerNum, uint16_t ping,
                               uint8_t clientType, uint8_t clientFlags);
void sdl3ImguiUpdatePlayerFlags(unsigned char playerNum, uint8_t clientType, uint8_t clientFlags);
/* Ping-only counterpart called from the per-tick snapshot apply path —
 * preserves the cached clientType/clientFlags that the full meta
 * updater would otherwise overwrite. */
void sdl3ImguiUpdatePlayerPing(unsigned char playerNum, uint16_t ping);

/*********************************************************
*NAME:          sdl3ImguiGetSteamIcon
*PURPOSE:
*  Returns the SDL_Texture for the Steam icon.  Loads the SVG
*  lazily on first call.  Returns NULL if it could not be loaded.
*  (The WBN-verified shield is drawn procedurally, not from a
*  texture — see imguiShieldBadge.)
*********************************************************/
SDL_Texture *sdl3ImguiGetSteamIcon(void);

/*********************************************************
*NAME:          sdl3ImguiGetBrainIcon
*PURPOSE:
*  Returns the SDL_Texture for the AI-brain icon (the badge
*  shown for bot players). Loads the SVG lazily on first
*  call. Returns NULL if the SVG could not be loaded.
*********************************************************/
SDL_Texture *sdl3ImguiGetBrainIcon(void);

/*********************************************************
*NAME:          sdl3ImguiPlayerIsBot
*PURPOSE:
*  Returns true if the given player slot is flagged as a
*  bot (PLAYER_FLAG_BOT) in the cached player list.
*********************************************************/
bool sdl3ImguiPlayerIsBot(unsigned char playerNum);

/*********************************************************
*NAME:          sdl3ImguiGetPlatformIcon
*PURPOSE:
*  Returns the SDL_Texture for the platform icon matching
*  clientType (a CLIENT_TYPE_* enum value). Loads the SVGs
*  lazily on first call. Returns NULL for CLIENT_TYPE_UNKNOWN
*  or out-of-range values. CLIENT_TYPE_WEB falls back to the
*  globe icon since no dedicated web.svg exists.
*********************************************************/
SDL_Texture *sdl3ImguiGetPlatformIcon(uint8_t clientType);

/*********************************************************
*NAME:          renderPlayerName
*PURPOSE:
*  Renders a player name with its decorations as a single
*  inline ImGui run: platform icon (gold-tinted if supporter)
*  then WBN verified shield then Steam icon to the left of
*  the name, optional country flag to the right when
*  showCountry is true and countryCode is a real ISO 3166
*  alpha-2 code. Steam badge surfaces for either a
*  WBN-linked Steam account or a player running the Steam
*  build of the client (intentional widening: previously
*  WBN-link only).
*
*ARGUMENTS:
*  name        - UTF-8 NUL-terminated display name. NULL or
*                "" emits just the icon sequence (icon-only
*                mode) — useful for surfaces that render the
*                name via their own widget (Selectable,
*                TextColored, etc.). In that mode
*                countryCode/showCountry are ignored.
*  flags       - PLAYER_FLAG_* bits (see players.h)
*  clientType  - CLIENT_TYPE_* enum — drives platform icon.
*                CLIENT_TYPE_UNKNOWN renders no platform icon.
*  countryCode - 2-letter ISO 3166 code, "" or "XX" for
*                unknown
*  showCountry - whether to render the country flag
*********************************************************/
void renderPlayerName(const char *name, uint8_t flags, uint8_t clientType,
                      const char *countryCode, bool showCountry);

/* Draws the country flag for an alpha-2 code and, on hover, a localized
 * country-name tooltip. Returns true iff a flag image was drawn (false for
 * the "XX" sentinel, a null/too-short code, or a missing SVG). Caller owns
 * layout (SameLine, cursor positioning). */
bool drawCountryFlagWithTip(const char *countryCode);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_H */

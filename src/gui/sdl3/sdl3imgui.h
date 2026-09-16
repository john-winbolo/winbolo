/*
 * Copyright (c) 1998-2026 John Morrison.
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

#include "platform_types.h"  /* PlayerBitMap */

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
/* What Ctrl/Cmd+M does. Raises the Send Message pop-out without ever hiding
 * it — the key press lands on the main window, so a toggle would close the
 * window the player meant to bring forward — but toggles the in-window panel
 * the full screen map draws instead, which has no window to be behind. */
void sdl3ImguiSendMsgShortcut(void);
void sdl3ImguiShowMapOverview(bool open);
/* In-window Map Overview: the map fills the game window and the window goes
 * fullscreen. Desktop only, like the pop-out above. */
void sdl3ImguiShowOverviewInWindow(bool active);
/* The one full screen command. In a game it is the full screen map above,
 * which carries the window full screen with it; outside one there is no map
 * to show, so it is the plain app full screen flag. Alt+Enter and the macOS
 * Window menu item both come through here so the two routes cannot drift. */
void sdl3ImguiToggleFullScreen(struct ClientSim *cs);
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
/* The map overview is a desktop-only pop-out with no in-window twin, so it
 * always reports closed in tablet mode. */
bool sdl3ImguiIsMapOverviewOpen(void);
bool sdl3ImguiIsOverviewInWindowOpen(void);

/* True while keyboard focus is on a window the player drives the game
 * from: the main window, or the Map Overview pop-out. The other pop-outs
 * are deliberately excluded — typing in Send Message must never steer the
 * tank. */
bool sdl3ImguiGameInputWindowHasFocus(void);

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
*NAME:          sdl3ImguiGetPlayerName
*PURPOSE:
*  The display name last pushed for a player slot, or ""
*  when the slot is empty or out of range. Same mirror the
*  Players menu draws from, so it tracks in-game name
*  changes. Never NULL; the pointer stays valid until the
*  next update for that slot.
*********************************************************/
const char *sdl3ImguiGetPlayerName(unsigned char playerNum);

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
*NAME:          sdl3ImguiGetBotIconSurface
*PURPOSE:
*  Returns the chip icon — the badge shown for a computer
*  player — as an SDL_Surface, renderer-free, so the
*  tank-label caches can texture it on whichever renderer
*  hosts them.
*
*  isAlly picks the artwork: the green chip for a player
*  allied with this client, the red one otherwise. The two
*  are separate files drawn as authored, not one shape
*  recoloured, and they keep their own colours — a tank
*  label alpha-mods its icon but never colour-mods it, the
*  same reason a country flag stays coloured out on the map.
*
*  Owned by this module; do not destroy. Both are loaded
*  lazily on first call and freed once in sdl3ImguiCleanup.
*  Returns NULL if the SVG could not be loaded.
*********************************************************/
SDL_Surface *sdl3ImguiGetBotIconSurface(bool isAlly);

/*********************************************************
*NAME:          sdl3ImguiPlayerIsAlly
*AUTHOR:        Andrew Roth
*CREATION DATE: 12/9/26
*LAST MODIFIED: 12/9/26
*PURPOSE:
* Is that player allied with this client? Answered from a mirror refreshed
* once a frame in sdl3ImguiPumpAndRender, so callers with no ClientSim can
* ask — the tank labels on the map are why it exists: they pick a bot's chip
* by it and are handed a player number and a font, nothing more.
*
* False for your own slot, and false with no game running.
*
*ARGUMENTS:
* playerNum - the slot to ask about
*********************************************************/
bool sdl3ImguiPlayerIsAlly(unsigned char playerNum);

#if defined(WINBOLO_VOICE)
/* Which voice glyph to rasterize. The shape says which end the state belongs
   to, the same way it does in the players panel: a speaker for what is played
   here, a microphone for what is captured at the other end. */
typedef enum {
    MIC_GLYPH_MIC,
    MIC_GLYPH_MIC_MUTED,
    MIC_GLYPH_SPEAKER,
    MIC_GLYPH_SPEAKER_MUTED
} MicIconGlyph;

/*********************************************************
*NAME:          sdl3ImguiCreateMicIconSurface
*PURPOSE:
*  Rasterizes one voice glyph at exactly the pixel size
*  asked for, for the two places that draw these from C at a
*  size that follows the window: the game view's own mute
*  indicator and the on-map tank labels. Neither wants any
*  scaling at draw time — the barred glyphs cut their slash
*  with a gap about a unit wide in the SVG's 24-unit
*  viewBox, and a downscale averages it away.
*
*  Unlike the …Get…Surface accessors above, the surface is
*  the CALLER'S: destroy it with SDL_DestroySurface once it
*  has been textured. size is clamped to a legible range.
*  Returns NULL if the SVG could not be loaded.
*
*ARGUMENTS:
*  glyph - which of the four to draw
*  size  - wanted width and height in pixels
*********************************************************/
SDL_Surface *sdl3ImguiCreateMicIconSurface(MicIconGlyph glyph, int size);

/*********************************************************
*NAME:          sdl3ImguiPlayerFlags
*PURPOSE:
*  Returns the cached PLAYER_FLAG_* bits for a slot, as the
*  server last published them, so a drawer outside this file
*  can resolve a player's voice state the way the players
*  panel does. Returns 0 for an out-of-range slot.
*
*ARGUMENTS:
*  playerNum - slot to read
*********************************************************/
uint8_t sdl3ImguiPlayerFlags(unsigned char playerNum);

/*********************************************************
*NAME:          sdl3ImguiPlayerIsSelf
*PURPOSE:
*  Returns true if the slot is the local player's. The slot
*  is read once a frame in sdl3ImguiPumpAndRender, which is
*  the only place with a ClientSim to ask; until the first
*  read, and whenever that runs without one, the answer is
*  false for every slot rather than for slot 0 by accident.
*
*ARGUMENTS:
*  playerNum - slot to test
*********************************************************/
bool sdl3ImguiPlayerIsSelf(unsigned char playerNum);
#endif

/*********************************************************
*NAME:          sdl3ImguiPlayerIsBot
*PURPOSE:
*  Returns true if the given player slot is flagged as a
*  bot (PLAYER_FLAG_BOT) in the cached player list.
*********************************************************/
bool sdl3ImguiPlayerIsBot(unsigned char playerNum);

/* Whether tank labels are the long kind, name and location. The bot chip
 * beside a name rides with the location: a short label is the bare name
 * for a person and a bot alike. */
bool sdl3ImguiTankLabelsLong(void);

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

/*********************************************************
*NAME:          RenderPlayerNameOpts
*AUTHOR:        Andrew Roth
*CREATION DATE: 12/9/26
*LAST MODIFIED: 14/9/26
*PURPOSE:
* The optional, per-call choices renderPlayerNameEx offers, in one struct so
* that a caller says everything it wants in the call itself.
*
* A struct and not two more parameters because this header has C linkage, so
* a default argument will not compile and every new choice would otherwise
* have to be spelled out at all seven call sites. Callers pass NULL for the
* plain behaviour, and the next flag added here changes neither the
* signature nor any call site that does not want it.
*
*ARGUMENTS:
* botIsAlly  - true draws the green bot chip instead of the red one. Which
*              chip a bot gets is the caller's to know: renderPlayerNameEx is
*              given a player's flags, not their slot, so it cannot work out
*              who is allied with whom, while the players panel and the
*              in-game player menu already compute that for the mark they
*              draw in front of the name. The green and red chips are
*              separate artwork, drawn as authored rather than one shape
*              recoloured: each is gold pins around a dark body with a
*              coloured die, and a tinted silhouette loses all of it.
* keepIconY  - true holds every badge in the run at the y the call started
*              at, instead of snapping back to the line's top on each
*              SameLine. For a caller that centres the run inside a row
*              taller than the icons: on the line's top the first icon lands
*              on the row's midline and the rest sit above it. A caller that
*              starts its run at the line's top sees no difference.
*********************************************************/
typedef struct RenderPlayerNameOpts {
    bool botIsAlly;   /* green chip rather than red for a bot */
    bool keepIconY;   /* hold every badge at the starting y */
} RenderPlayerNameOpts;

/*********************************************************
*NAME:          renderPlayerNameEx
*AUTHOR:        Andrew Roth
*CREATION DATE: 12/9/26
*LAST MODIFIED: 14/9/26
*PURPOSE:
* renderPlayerName with the per-call choices in RenderPlayerNameOpts. This
* is where the drawing actually happens; renderPlayerName is the same call
* with no options.
*
*ARGUMENTS: as renderPlayerName, plus
*  opts - the choices for this call, or NULL for both false
*********************************************************/
void renderPlayerNameEx(const char *name, uint8_t flags, uint8_t clientType,
                        const char *countryCode, bool showCountry,
                        const RenderPlayerNameOpts *opts);

#if defined(WINBOLO_VOICE)
/*********************************************************
*NAME:          renderPlayerMicCell
*PURPOSE:
*  Renders one player's voice cell for a player row: a
*  size x size icon whose shape and tint reflect the
*  player's voice state, resolved in precedence order
*  (muted by this client, no microphone, talking, muted
*  their own microphone, idle), with a tooltip naming that
*  state. A talking player's speaker is drawn dim and filled
*  from the bottom, in the talking colour, to how loud they
*  are right now — the glyph is the meter, so a filled cell
*  is the same size as an empty one.
*
*  The shape says which end the state belongs to. A speaker
*  for the states about playback here — a remote player
*  idle, talking, or muted by this client, which is what
*  clicking the cell changes — and a microphone for the two
*  about capture at the other end, no microphone and muted
*  their own. Every state on the local player's own row is
*  about this client's own capture, so that row is
*  microphones throughout.
*
*  On another player's row the icon is a button that toggles
*  this client's mute of that player, locally and on the
*  server; on the local player's own row it is a plain
*  image. If the icon texture failed to load a blank of the
*  same size holds the column. Loads the icon textures on
*  first use. Draws only — the caller owns layout
*  (SameLine, cursor positioning).
*
*ARGUMENTS:
*  cs          - client sim, for the server-side mute send
*  playerNum   - player slot (0..MAX_PLAYERS-1); also keys
*                the ImGui id so each row's button is unique
*  clientFlags - that player's PLAYER_FLAG_* bits (HAS_MIC,
*                VOICE_MUTED)
*  talkingMap  - PlayerBitMap of players producing voice now
*  isSelf      - true when playerNum is the local player
*  size        - icon edge length in pixels
*  inLobby     - true from the lobby table, false from the
*                in-game players panel. Decides whether a
*                remote player with no microphone is drawn at
*                all: in game that icon is clutter, so the
*                cell is left blank and only holds its width,
*                while in the lobby knowing that someone
*                cannot talk is the point. A player muted by
*                this client is drawn either way — that state
*                outranks it.
*********************************************************/
void renderPlayerMicCell(struct ClientSim *cs, int playerNum, uint8_t clientFlags,
                         PlayerBitMap talkingMap, bool isSelf, float size,
                         bool inLobby);
#endif

/*********************************************************
*NAME:          renderPlayerPingMuteCell
*PURPOSE:
*  Renders one player's smart-ping mute toggle for a player
*  row, beside the voice cell: a size x size button that
*  hides or shows that player's smart pings for this client,
*  locally and on the server (CMD_PLAYER_PING_MUTE). Muting
*  is independent of the voice/chat mute, so this is not
*  gated on WINBOLO_VOICE. The local player's own row draws a
*  blank of the same size — you always see your own pings.
*  Draws only; the caller owns layout (SameLine, sizing).
*ARGUMENTS:
*  cs        - client sim, for the server-side mute send and
*              the reflected client-side muted state
*  playerNum - player slot; also keys the ImGui id
*  isSelf    - true when playerNum is the local player
*  size      - cell edge length in pixels
*********************************************************/
void renderPlayerPingMuteCell(struct ClientSim *cs, int playerNum, bool isSelf,
                              float size);

/* Draws the country flag for an alpha-2 code and, on hover, a localized
 * country-name tooltip. Returns true iff a flag image was drawn (false for
 * the "XX" sentinel, a null/too-short code, or a missing SVG). Caller owns
 * layout (SameLine, cursor positioning). */
bool drawCountryFlagWithTip(const char *countryCode);

#ifdef __cplusplus
}
#endif

#endif /* SDL3IMGUI_H */

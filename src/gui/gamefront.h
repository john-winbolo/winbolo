/*
 * $Id$
 *
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
*Name:          Game Front
*Filename:      gamefront.h
*Author:        John Morrison
*Creation Date: 27/01/99
*Last Modified: 24/06/02
*Purpose:
*  Provides the front end for viewing maps
*********************************************************/

#ifndef GAMEFRONT_H
#define GAMEFRONT_H

#include "global.h"
#include "client_enums.h"  /* aiType, gameType */
#include "server_sim.h"
#include "input.h"
#include "winbolo.h"
#include "visibility_presets.h"  /* VisibilitySettings / VisibilityPreset */
#include "../winbolonet/winbolonet_client.h"  /* WbnStats */


/* Default keys — SDL_Scancode values (USB HID page 07) */
#define DEFAULT_FORWARD      8    /* SDL_SCANCODE_E */
#define DEFAULT_BACKWARD     7    /* SDL_SCANCODE_D */
#define DEFAULT_LEFT         22   /* SDL_SCANCODE_S */
#define DEFAULT_RIGHT        9    /* SDL_SCANCODE_F */
#define DEFAULT_SHOOT        44   /* SDL_SCANCODE_SPACE */
#define DEFAULT_LAY_MINE     225  /* SDL_SCANCODE_LSHIFT */

/* Scroll — arrow keys (not everyone has a numeric keypad) */
#define DEFAULT_SCROLLLEFT   80   /* SDL_SCANCODE_LEFT */
#define DEFAULT_SCROLLUP     82   /* SDL_SCANCODE_UP */
#define DEFAULT_SCROLLRIGHT  79   /* SDL_SCANCODE_RIGHT */
#define DEFAULT_SCROLLDOWN   81   /* SDL_SCANCODE_DOWN */

/* View keys */
#define DEFAULT_TANKVIEW     23   /* SDL_SCANCODE_T */
#define DEFAULT_PILLVIEW     10   /* SDL_SCANCODE_G */
#define DEFAULT_ALLYVIEW     28   /* SDL_SCANCODE_Y */
#define DEFAULT_BASEVIEW     16   /* SDL_SCANCODE_M */

/* Held while the wheel turns over the map overview, it zooms the map rather
   than moving the gunsight. Cleared (0), the wheel always zooms there. */
#define DEFAULT_OVERVIEW_ZOOM 224 /* SDL_SCANCODE_LCTRL */

/* The map overview's own camera keys: the follow / free toggle and the two
   ends of the zoom ladder. */
#define DEFAULT_OVERVIEW_FOLLOW   6   /* SDL_SCANCODE_C */
#define DEFAULT_OVERVIEW_ZOOMIN   46  /* SDL_SCANCODE_EQUALS */
#define DEFAULT_OVERVIEW_ZOOMOUT  45  /* SDL_SCANCODE_MINUS */

/* Quick-build keys — number row 1-5 */
#define DEFAULT_QUICKTREE    30   /* SDL_SCANCODE_1 */
#define DEFAULT_QUICKROAD    31   /* SDL_SCANCODE_2 */
#define DEFAULT_QUICKWALL    32   /* SDL_SCANCODE_3 */
#define DEFAULT_QUICKPILLBOX 33   /* SDL_SCANCODE_4 */
#define DEFAULT_QUICKMINE    34   /* SDL_SCANCODE_5 */

/* Smart ping — the pie menu opens on a chord, so these are packed
   pingBinding ints (src/gui/sdl3/ping_binding.h) rather than bare scancodes.
   Ctrl and Alt with the right mouse button both open it out of the box,
   because the right button is otherwise unused in the game view and the two
   modifiers cover both muscle memories. The third slot starts unbound, and so
   does every direct-ping slot: a chord that fires a ping with no menu at all
   is a thing the player asks for, not something to hand them.
   0x0200 + 3 is the right mouse button; 0x10000 / 0x20000 are Ctrl / Alt. */
#define DEFAULT_PING1  (0x00010000 | (512 + 3))  /* Ctrl + Right Mouse */
#define DEFAULT_PING2  (0x00020000 | (512 + 3))  /* Alt + Right Mouse  */
#define DEFAULT_PING3  0

/* Gunsight range — KP+ / Enter */
#define DEFAULT_SCROLL_GUNINCREASE  87   /* SDL_SCANCODE_KP_PLUS */
#define DEFAULT_SCROLL_GUNDECREASE  40   /* SDL_SCANCODE_RETURN */
#define DEFAULT_SCROLL_PILLVIEW     10   /* SDL_SCANCODE_G */

/* Voice — both bound to nothing else, and both under the left hand. Q sits
   beside the movement keys (E/D/S/F) because push to talk is held while
   driving; Z is a corner away because the mute toggle is pressed once and
   wants to be hard to hit by accident. */
#define DEFAULT_PUSHTOTALK   20   /* SDL_SCANCODE_Q */
#define DEFAULT_MUTEMIC      29   /* SDL_SCANCODE_Z */

#define PREFERENCE_FILE "WinBolo.ini"

#define TRACKER_ADDRESS "tracker.winbolo.com"
#define TRACKER_PORT 50000
#define TRACKER_ENABLE FALSE

#define YESNO_TO_TRUEFALSE(X) (tolower(X) == 'y' ? 1 : 0)
#define TRUEFALSE_TO_STR(X) (X == TRUE ? "Yes" : "No")

/* Per-bot configuration for single-player setup */
#define MAX_BOT_SLOTS 15

typedef struct {
    char brainPath[FILENAME_MAX];
    uint8_t teamNumber;  /* 0 = unassigned, 1-16 */
} GameFrontBotSlot;

typedef struct {
    int count;
    uint8_t playerTeamNumber;  /* Team for the human player (0 = unassigned, 1-16) */
    GameFrontBotSlot bots[MAX_BOT_SLOTS];
} GameFrontBotSetup;

/* Opening Dialog State machine */
typedef enum {
  openStart,
  openLang,   /* Language selection dialog */
  openWelcome,
  openTutorial,
  openSetup,
  openUdp,
  openUdpJoin,
  openUdpManual,
  openUdpSetup,
  openLan,
  openLanJoin,
  openLanManual,
  openLanSetup,
  openInternet,
  openInternetJoin,
  openInternetManual,
  openInternetSetup,
  openFinished,
  openSettings,
  openMapEditor,
  openLogViewer,
  openSpectate   /* Spectate the selected browser server via the modal host */
} openingStates;

/*********************************************************
*NAME:          gameFrontStart
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED: 11/6/00
*PURPOSE:
*  Handles opening dialog boxes and starts up game
*  subsystems. Returns success.
*
*ARGUMENTS:
*  cmdLine  - Command line
*  keys     - Structure that holds the keys
*  isLoaded - Have we loaded before?
*********************************************************/
bool gameFrontStart(const char *cmdLine, keyItems *keys, bool isLoaded, struct ClientSim **out_cs);

/*********************************************************
*NAME:          gameFrontEnd
*AUTHOR:        John Morrison
*CREATION DATE: 27/10/98
*LAST MODIFIED:  11/6/00
*PURPOSE:
*  Handles game shutdown.
*
*ARGUMENTS:
*  keys       - Pointer to hold Key Preferences
*  gamePlayed - TRUE if we actually entered the main
*               screen (ie played a game)
*  isQuiting  - TRUE if we are quiting
*********************************************************/
void gameFrontEnd(keyItems *keys, bool gamePlayed, bool isQuiting);

/*********************************************************
*NAME:          gameFrontSaveTankPrefs
*PURPOSE:
*  Copies auto-slowdown and auto-hide-gunsight from the
*  active tank into the frontend globals so they survive
*  a return-to-lobby cycle (which skips gameFrontEnd).
*
*ARGUMENTS:
*  cs - Active ClientSim (may be NULL)
*********************************************************/
void gameFrontSaveTankPrefs(struct ClientSim *cs);

/*********************************************************
*NAME:          gameFrontSetDlgState
*AUTHOR:        John Morrison
*CREATION DATE: 27/1/99
*LAST MODIFIED:  6/7/00
*PURPOSE:
*  Changes the dialog state machine
*
*ARGUMENTS:
*  newState - Handle to the app instance
*********************************************************/
bool gameFrontSetDlgState(openingStates newState);

/*********************************************************
*NAME:          gameFrontGetCmdArg
*AUTHOR:        John Morrison
*CREATION DATE: 28/1/99
*LAST MODIFIED: 28/1/99
*PURPOSE:
* Gets the command line argument (or last map used)
*
*ARGUMENTS:
*  getName - Holds the commandline argument
*********************************************************/
void gameFrontGetCmdArg(char *getName);

/*********************************************************
*NAME:          gameFrontSetFileName
*AUTHOR:        John Morrison
*CREATION DATE: 28/1/99
*LAST MODIFIED: 28/1/99
*PURPOSE:
* Sets the map to use. Maps have been verified as OK
*
*ARGUMENTS:
*  getName - Holds the filename
*********************************************************/
void gameFrontSetFileName(char *getName);

/*********************************************************
*NAME:          gameFrontSetGameOptions
*AUTHOR:        John Morrison
*CREATION DATE: 28/1/99
*LAST MODIFIED: 3/12/99
*PURPOSE:
* Sets the game options up.
*
*ARGUMENTS:
*  pword    - Holds the password. (Empty if none)
*  gtype    - Holds the game type.
*  hm       - Are hidden mines allowed?
*  ai       - Are computer tanks allowed etc.
*  sd       - Game start delay
*  tlimit   - Game time limit
*  justPass - TRUE if we want to just set the password
*********************************************************/
void gameFrontSetGameOptions(char *pword, gameType gt, bool hm, aiType ai, int32_t sd, int32_t tlimit, bool justPass);

/*********************************************************
*NAME:          gameFrontGetGameOptions
*AUTHOR:        John Morrison
*CREATION DATE: 19/4/99
*LAST MODIFIED: 19/4/99
*PURPOSE:
* Gets the game options.
*
*ARGUMENTS:
*  pword  - Holds the password. (Empty if none)
*  gtype  - Holds the game type.
*  hm     - Are hidden mines allowed?
*  ai     - Are computer tanks allowed etc.
*  sd     - Game start delay
*  tlimit - Game time limit
*********************************************************/
void gameFrontGetGameOptions(char *pword, gameType *gt, bool *hm, aiType *ai, int32_t *sd, int32_t *tlimit);

/*********************************************************
*NAME:          gameFrontSetBotOptions
*PURPOSE:
* Sets the bot options (count and brain path) from the
* game setup dialog.
*
*ARGUMENTS:
*  count     - Number of bots (0-15)
*  brainPath - Path to brain init.lua (NULL or "" for auto)
*********************************************************/
void gameFrontSetBotOptions(int count, const char *brainPath);

/*********************************************************
*NAME:          gameFrontGetBotOptions
*PURPOSE:
* Gets the bot options for the game setup dialog.
*
*ARGUMENTS:
*  count         - Pointer to hold bot count
*  brainPath     - Buffer to hold brain path
*  brainPathSize - Size of brainPath buffer
*********************************************************/
void gameFrontGetBotOptions(int *count, char *brainPath, size_t brainPathSize);

/*********************************************************
*NAME:          gameFrontSetBotSetup
*PURPOSE:
* Sets per-bot configuration (brain path and team per bot)
* from the single-player game setup dialog.
*
*ARGUMENTS:
*  setup - Pointer to bot setup structure
*********************************************************/
void gameFrontSetBotSetup(const GameFrontBotSetup *setup);

/*********************************************************
*NAME:          gameFrontGetBotSetup
*PURPOSE:
* Gets per-bot configuration for the game setup dialog.
*
*ARGUMENTS:
*  setup - Pointer to hold bot setup
*********************************************************/
void gameFrontGetBotSetup(GameFrontBotSetup *setup);

/*********************************************************
*NAME:          gameFrontGetUdpOptions
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Gets the UDP options
*
*ARGUMENTS:
*  pn       - Pointer to hold the player name
*  add      - Pointer to hold target machine address
*  theirUdp - Pointer to hold target machine UDP port
*  myUdp    - Pointer to this machine UDP port
*********************************************************/
void gameFrontGetUdpOptions(char *pn, char *add, unsigned short *theirUdp, unsigned short *myUdp);

/*********************************************************
*NAME:          gameFrontSetUdpOptions
*AUTHOR:        John Morrison
*CREATION DATE: 21/2/99
*LAST MODIFIED: 21/2/99
*PURPOSE:
* Gets the UDP options
*
*ARGUMENTS:
*  pn       - Player name
*  add      - Target machine address
*  theirUdp - Target machine UDP port
*  myUdp    - This machine UDP port
*********************************************************/
void gameFrontSetUdpOptions(char *pn, char *add, unsigned short theirUdp, unsigned short myUdp);

/*********************************************************
*NAME:          gameFrontGetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 24/2/99
*LAST MODIFIED: 24/2/99
*PURPOSE:
* Gets the player name
*
*ARGUMENTS:
*  pn       - Pointer to hold the player name
*********************************************************/
void gameFrontGetPlayerName(char *pn);

/*********************************************************
*NAME:          gameFrontSetPlayerName
*AUTHOR:        John Morrison
*CREATION DATE: 24/2/99
*LAST MODIFIED:  2/6/00
*PURPOSE:
* Sets the player name
*
*ARGUMENTS:
*  pn       - Player name to set to
*********************************************************/
void gameFrontSetPlayerName(char *pn);

/*********************************************************
*NAME:          gameFrontSetAIType
*AUTHOR:        John Morrison
*CREATION DATE: 26/2/99
*LAST MODIFIED: 26/2/99
*PURPOSE:
* Sets the AI type of the game being joined, on the client sim and
* the brains menu. (From networking module.) It does not touch the
* saved computer tanks pick that gameFrontRememberAiPolicy records:
* a joined game's setting is not what the next hosted game opens on.
*
*ARGUMENTS:
*
*********************************************************/
void gameFrontSetAIType(aiType ait);

/*********************************************************
*NAME:          gameFrontGetPrefs
*AUTHOR:        John Morrison
*CREATION DATE: 19/4/99
*LAST MODIFIED:  4/1/00
*PURPOSE:
* Gets the preferences from the preferences file. Returns
* success.
*
*ARGUMENTS:
*  keys - Pointer to keys structure
*  useAutoslow - Pointer to hold auto slowdown
*  useAutohide - Pointer to hold auto gunsight show/hide
*********************************************************/
bool gameFrontGetPrefs(keyItems *keys, bool *useAutoslow, bool *useAutohide);

/*********************************************************
*NAME:          gameFrontPutPrefs
*AUTHOR:        John Morrison
*CREATION DATE: 19/4/99
*LAST MODIFIED: 19/4/99
*PURPOSE:
* Puts the preferences to the preferences file.
*
*ARGUMENTS:
*  keys       - Pointer to keys structure
*********************************************************/
void gameFrontPutPrefs(keyItems *keys);

/*********************************************************
*NAME:          gameFrontSaveCurrentPrefs
*PURPOSE:
* Convenience wrapper: snapshot the current key bindings via
* windowGetKeys and write the full prefs file. Use from
* user-driven toggle handlers so changes persist immediately
* instead of only on shutdown.
*********************************************************/
void gameFrontSaveCurrentPrefs(void);

/*********************************************************
*NAME:          gameFrontSaveWindowSettings
*AUTHOR:        Andrew Roth
*CREATION DATE: 19/4/26
*LAST MODIFIED: 19/4/26
*PURPOSE:
* Lightweight save of just window position/size settings.
* Called on every resize/move so window state persists.
*
*ARGUMENTS:
*  none
*********************************************************/
void gameFrontSaveWindowSettings(void);

/*********************************************************
*NAME:          gameFrontFlushWindowSettings
*PURPOSE:
* Write the window settings now, bypassing the 500ms
* debounce. For close/shutdown paths, where there may be
* no further gameFrontPumpDirty call to flush a trailing
* move/resize. Cheap and idempotent.
*
*ARGUMENTS:
*  none
*********************************************************/
void gameFrontFlushWindowSettings(void);

/*********************************************************
*NAME:          gameFrontPumpDirty
*PURPOSE:
* Consume point for the debounce dirty flag set by
* gameFrontSaveWindowSettings. Call once per frame from
* the main event/render loop — when a window-move/resize
* burst settles inside the 500ms throttle window, this is
* what flushes the trailing event so the final position
* survives. Cheap when nothing is dirty.
*********************************************************/
void gameFrontPumpDirty(void);

/*********************************************************
*NAME:          gameFrontStartPrefsSync
*PURPOSE:
* On sign-in, launch exactly one cloud-preferences sync per
* session off a worker thread (gated, no-op if already run or
* if no WBN token). Captures the upload snapshot and sync state
* on the calling (main) thread.
*********************************************************/
void gameFrontStartPrefsSync(void);

/*********************************************************
*NAME:          gameFrontPumpPrefsSync
*PURPOSE:
* Call once per frame (driven from gameFrontPumpDirty). When the
* sync worker has finished, joins it and applies the outcome on
* the main thread: adopts + live-applies a downloaded document,
* records a pushed version, or signs out on re-auth.
*********************************************************/
void gameFrontPumpPrefsSync(void);

/*********************************************************
*NAME:          gameFrontResetPrefsSyncSession
*PURPOSE:
* Clear the once-per-session sync gate so a later sign-in syncs
* again. Called from the logout path.
*********************************************************/
void gameFrontResetPrefsSyncSession(void);

/*********************************************************
*NAME:          gameFrontSetRemeber
*AUTHOR:        John Morrison
*CREATION DATE: 19/4/99
*LAST MODIFIED: 19/4/99
*PURPOSE:
* Sets whether we remeber the player name or not
*
*ARGUMENTS:
*  isSet - Value to set it to
*********************************************************/
void gameFrontSetRemeber(bool isSet);

/*********************************************************
*NAME:          gameFrontGetRemeber
*AUTHOR:        John Morrison
*CREATION DATE: 19/4/99
*LAST MODIFIED: 19/4/99
*PURPOSE:
* Gets whether we remeber the player name or not
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontGetRemeber(void);

/*********************************************************
*NAME:          gameFrontGetShowTutorialButton
*PURPOSE:
* Returns whether the Tutorial entry should appear on the welcome
* menu. TRUE on a fresh install; flipped to FALSE automatically when
* the player completes the tutorial; can be re-enabled from Settings.
*********************************************************/
bool gameFrontGetShowTutorialButton(void);

/*********************************************************
*NAME:          gameFrontSetShowTutorialButton
*PURPOSE:
* Sets the Tutorial-on-main-menu visibility and persists it to the
* INI file immediately so the change survives a crash or hard quit.
*********************************************************/
void gameFrontSetShowTutorialButton(bool show);

/*********************************************************
*NAME:          gameFrontGetLanguageCode
*PURPOSE:
* Reads the persisted BCP-47 language code (e.g. "en", "de", "pt-br")
* into out. Empty string if the user has never picked a language; the
* caller should run langAutoDetect() in that case.
*********************************************************/
void gameFrontGetLanguageCode(char *out, int outSize);

/*********************************************************
*NAME:          gameFrontSetLanguageCode
*PURPOSE:
* Persists the BCP-47 language code in the INI file. Pass an empty
* string to clear the saved preference (and revert auto-detect on
* the next launch).
*********************************************************/
void gameFrontSetLanguageCode(const char *code);

/*********************************************************
*NAME:          gameFrontRequestPlayTutorial
*PURPOSE:
* Settings dialog calls this when the user clicks "Play Tutorial".
* The next gameFrontConsumePlayTutorialRequest() call returns TRUE
* and clears the flag, so the openSettings handler can route to
* openTutorial instead of falling back to openWelcome.
*********************************************************/
void gameFrontRequestPlayTutorial(void);
bool gameFrontConsumePlayTutorialRequest(void);

/*********************************************************
*NAME:          gameFrontRequestUdpAutoJoin
*PURPOSE:
* Set when a Steam "join game" request arrives. The next
* gameFrontConsumeUdpAutoJoinRequest() call (made by the UDP
* setup dialog as it opens) returns TRUE and clears the flag,
* so the dialog auto-fires its Join button after a brief
* visible dwell instead of waiting for a manual click.
*********************************************************/
void gameFrontRequestUdpAutoJoin(void);
bool gameFrontConsumeUdpAutoJoinRequest(void);

/*********************************************************
*NAME:          gameFrontRequestTransition
*PURPOSE:
* Posts a state transition the welcome dialog will pick up on
* its next poll iteration and treat as if the equivalent ghost
* button was clicked. Used by host-OS shims (e.g. the macOS
* Dock menu) to trigger welcome-screen actions from outside the
* in-window UI. Must be called from the main thread — the
* welcome loop reads the channel on the same thread.
*********************************************************/
void gameFrontRequestTransition(openingStates s);
bool gameFrontConsumeRequestedTransition(openingStates *out);

/*********************************************************
*NAME:          gameFrontIsAtWelcome
*PURPOSE:
* TRUE while gameFrontDialogs() is sitting inside the
* welcomeShow() poll loop. Lets host-OS menus dim items
* that only make sense from the welcome screen.
*********************************************************/
bool gameFrontIsAtWelcome(void);

/*********************************************************
*NAME:          gameFrontSetupServer
*AUTHOR:        John Morrison
*CREATION DATE: 3/11/99
*LAST MODIFIED: 3/11/99
*PURPOSE:
* Attempts to start the server process. Returns success
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontSetupServer(void);

/*********************************************************
*NAME:          gameFrontGetTrackerOptions
*AUTHOR:        John Morrison
*CREATION DATE: 13/11/99
*LAST MODIFIED: 13/11/99
*PURPOSE:
* Gets the tracker options
*
*ARGUMENTS:
*  address - Buffer to hold address
*  port    - Pointer to hold port
*  enabled - Pointer to hold enabled flag
*********************************************************/
void gameFrontGetTrackerOptions(char *address, unsigned short *port, bool *enabled);

/*********************************************************
*NAME:          gameFrontSetTrackerOptions
*AUTHOR:        John Morrison
*CREATION DATE: 13/11/99
*LAST MODIFIED: 13/11/99
*PURPOSE:
* Sets the tracker options
*
*ARGUMENTS:
*  address - New tracker address
*  port    - New tracker port
*  enabled - New tracker enabled flag
*********************************************************/
void gameFrontSetTrackerOptions(char *address, unsigned short port, bool enabled);

/*********************************************************
*NAME:          gameFrontEnableRejoin
*AUTHOR:        John Morrison
*CREATION DATE: 22/6/00
*LAST MODIFIED: 22/6/00
*PURPOSE:
* Sets it so we want rejoin
*
*ARGUMENTS:
*
*********************************************************/
void gameFrontEnableRejoin(void);

/*********************************************************
*NAME:          gameFrontPreferencesExist
*AUTHOR:        John Morrison
*CREATION DATE: 13/12/99
*LAST MODIFIED: 13/12/99
*PURPOSE:
* Returns whether the preferences file exists or not.
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontPreferencesExist(void);

/* First-run online onboarding flag, backed by the SETTINGS /
 * "Onboarding Complete" preference (Yes/No). */
bool gameFrontOnboardingComplete(void);
void gameFrontSetOnboardingComplete(void);

/* Player's explicitly-chosen bot difficulty (lobby wrench dropdown), a
 * BOT_DIFFICULTY_* value persisted by name under BOT / "Chosen Difficulty".
 * It overrides the single-player skill guess from then on. The getter
 * returns false (leaving *out alone) until the player first chooses one; it
 * also migrates the older BOT / "Chosen Brain" pref, where the one gentle
 * brain meant Easy and any other meant Hard. */
void gameFrontSetChosenBotDifficulty(uint8_t difficulty);
bool gameFrontGetChosenBotDifficulty(uint8_t *out);

/* Player's explicitly-chosen bot MODE + level, as the brain's own manifest
 * keys (brains/<brain>/modes.txt). Stored under BOT / "Chosen Mode" and
 * BOT / "Chosen Difficulty"; for the default mode the level keys are the
 * same easy/medium/hard words the difficulty-only preference always held,
 * so an existing prefs file is honoured unchanged. Both getters return
 * false (and write "") when nothing has been chosen yet. */
void gameFrontSetChosenBotModeAndLevel(const char *modeKey,
                                       const char *levelKey);
bool gameFrontGetChosenBotModeKey(char *out, size_t outSz);
bool gameFrontGetChosenBotLevelKey(char *out, size_t outSz);

/* The colour the lobby paints a bot's name tag with, remembered per bot name
 * ("GoalHunter") in the prefs as "#RRGGBB" so a bot that declares no colour
 * of its own still looks the same every launch. Get returns false when none
 * is remembered yet. */
bool gameFrontGetBotTagColor(const char *botName, uint32_t *rgb);
void gameFrontSetBotTagColor(const char *botName, uint32_t rgb);

/* The difficulty a single-player bot should be created with: the player's
 * own chosen difficulty when they have picked one, else the skill guess —
 * Hard when signed in to WinBolo.net with more than 5 games on record, Easy
 * for everybody else. One place so the auto-seeded bots and the lobby's
 * Add Bot agree. Returns a BOT_DIFFICULTY_* value. */
uint8_t gameFrontSpBotDifficulty(void);

/* The same answer widened to modes: which of the brain's declared modes an
 * SP bot runs in (the player's chosen mode when that brain still has it,
 * else 0 — the default mode), and the level index inside it (the skill
 * guess for mode 0, the player's chosen level key otherwise, falling back
 * to the mode's own default). brainPath is the bot's init.lua path. */
uint8_t gameFrontSpBotMode(const char *brainPath);
uint8_t gameFrontSpBotLevel(const char *brainPath, uint8_t mode);

/*********************************************************
*NAME:          gameFrontSetWinbolonetToken
*PURPOSE:
* Stores a WinBolo.net auth token and its expiry.
*
*ARGUMENTS:
* token  - 64-char hex auth token
* expiry - Expiry datetime string (Y-m-d H:i:s)
*********************************************************/
void gameFrontSetWinbolonetToken(const char *token, const char *expiry);

/*********************************************************
*NAME:          gameFrontGetWinbolonetToken
*PURPOSE:
* Gets the stored WinBolo.net auth token and expiry.
*
*ARGUMENTS:
* token  - Destination for token string
* expiry - Destination for expiry string
*********************************************************/
void gameFrontGetWinbolonetToken(char *token, char *expiry);

/*********************************************************
*NAME:          gameFrontClearWinbolonetToken
*PURPOSE:
* Clears the stored WinBolo.net token (logout).
*********************************************************/
void gameFrontClearWinbolonetToken(void);

/* How the current WinBolo.net session was authenticated: "steam",
 * "password", or "" when not signed in. Device-local (WINBOLO.NET
 * section), never synced. */
void gameFrontSetWbnAuthMethod(const char *method);
void gameFrontGetWbnAuthMethod(char *out, size_t outSize);

/* Sticky record of an explicit user sign-out. Set when the player signs
 * out, cleared only when they explicitly sign back in. While set, the
 * silent re-auth paths (welcome-screen launch re-auth, join-time Steam
 * auto-auth) and any auth worker landing afterwards must not sign the
 * player back in. Device-local (WINBOLO.NET section), never synced. */
void gameFrontSetWbnSignedOut(bool signedOut);
bool gameFrontGetWbnSignedOut(void);

/* Acquire a Steam auth-session ticket and hex-encode it into outHex
 * (must hold at least 2049 bytes). Returns true on success. Must be
 * called on the main thread (touches the Steam API). */
bool gameFrontGetSteamTicketHex(char *outHex, size_t outSize);

/* Apply a successful WinBolo.net Steam-auth response: store the token
 * (auth method "steam"), rank and stats; seed the player name from the
 * Steam persona only when no name is set yet (an existing name is kept);
 * and trigger the once-per-session cloud prefs sync. */
void gameFrontApplySteamAuthResult(const char *token, const char *expiry,
                                   const char *playerName, int rank,
                                   int rankTotal, const WbnStats *stats);

/*********************************************************
*NAME:          gameFrontGetWinbolonetUse
*PURPOSE:
* Returns whether WinBolo.net is active (token exists).
*********************************************************/
bool gameFrontGetWinbolonetUse(void);

/*********************************************************
*NAME:          gameFrontSetWinbolonetRank
*PURPOSE:
* Stores the player's WinBolo.net 1v1 ladder position.
* rank is -1 when unranked; rankTotal is the ranked-player
* count. Written by both auth paths, read by the UI.
*********************************************************/
void gameFrontSetWinbolonetRank(int rank, int rankTotal);

/*********************************************************
*NAME:          gameFrontGetWinbolonetRank
*PURPOSE:
* Returns the stored ladder position. rank is -1 when
* unranked/unknown. Either out-param may be NULL.
*********************************************************/
void gameFrontGetWinbolonetRank(int *rank, int *rankTotal);

/*********************************************************
*NAME:          gameFrontSetWinbolonetStats
*PURPOSE:
* Stores the player's per-mode WinBolo.net play stats from
* the last auth/validate response. Cleared on sign-out.
*********************************************************/
void gameFrontSetWinbolonetStats(const WbnStats *s);

/*********************************************************
*NAME:          gameFrontGetWinbolonetStats
*PURPOSE:
* Returns the stored per-mode play stats. `valid` is FALSE
* when no stats have been captured this session.
*********************************************************/
void gameFrontGetWinbolonetStats(WbnStats *out);

/*********************************************************
*NAME:          gameFrontIsSupporter
*PURPOSE:
* Returns TRUE when the local player has Supporter status —
* the same signal applied to the in-game self badge. Used to
* gold-tint the WinBolo.net shield on the welcome screen.
*********************************************************/
bool gameFrontIsSupporter(void);

/*********************************************************
*NAME:          gameFrontSetRegistryKeys
*AUTHOR:        John Morrison
*CREATION DATE: 30/08/02
*LAST MODIFIED: 30/08/02
*PURPOSE:
* Sets the winbolo registry keys for winbolo:// game
* opening
*
*ARGUMENTS:
*********************************************************/
void gameFrontSetRegistryKeys(void);

/*********************************************************
*NAME:          gameFrontSetAddressFromWebLink
*AUTHOR:        John Morrison
*CREATION DATE: 30/08/02
*LAST MODIFIED: 30/08/02
*PURPOSE:
* Sets the winbolo connect IP and port from a winbolo://
* URL link
*
*ARGUMENTS:
* address - Address passed to winbolo
*********************************************************/
void gameFrontSetAddressFromWebLink(char *address);

/*********************************************************
*NAME:          gameFrontHandleUrlOpen
*PURPOSE:
* Handles a winbolo:// URL received while the app is
* already running (e.g. via SDL_EVENT_DROP_FILE on macOS).
* Sets the address/port and navigates to the connect dialog.
*
*ARGUMENTS:
* url - Full winbolo:// URL string
*********************************************************/
void gameFrontHandleUrlOpen(char *url);

/* Update Steam rich presence with current map/player info.
 * Called after joining a game or exiting the lobby. */
void gameFrontUpdateSteamPresence(struct ClientSim *cs);

/* Set Steam rich presence for the main menu / welcome screen.
 * Clears any stale map/numplayers/connect tokens. */
void gameFrontSetSteamPresenceMenu(void);

/* Set Steam rich presence for the lobby screen.
 * Includes map name, player count, and a connect string so
 * friends can join the same lobby via Steam. */
void gameFrontSetSteamPresenceLobby(struct ClientSim *cs);

/* Throttled per-frame refreshers for the lobby and in-game loops. Safe to
 * call every frame; they push to Steam at most a few times per second so the
 * player count and (for hosts) the resolved external address stay current
 * without hitting Steam's rich-presence rate limit. */
void gameFrontTickSteamPresenceLobby(struct ClientSim *cs);
void gameFrontTickSteamPresenceGame(struct ClientSim *cs);

/*********************************************************
*NAME:          gameFrontReloadSkins
*AUTHOR:        John Morrison
*CREATION DATE: 28/09/03
*LAST MODIFIED: 28/09/03
*PURPOSE:
* Restarts draw and sound subsystems to reload a new skin
*
*ARGUMENTS:
*
*********************************************************/
void gameFrontReloadSkins(void);

/*********************************************************
*NAME:          gameFrontShutdownServer
*AUTHOR:        John Morrison
*CREATION DATE: 11/12/03
*LAST MODIFIED: 11/12/03
*PURPOSE:
* Stops the server if we are hosting
*
*ARGUMENTS:
*
*********************************************************/
void gameFrontShutdownServer(void);

/*********************************************************
*NAME:          gameFrontHasLocalServer
*PURPOSE:
* TRUE while this process owns a local ServerSim (single
* player or a listen server). The round-log accessors only
* describe a round this process recorded, so a caller must
* not offer them otherwise.
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontHasLocalServer(void);

/*********************************************************
*NAME:          gameFrontSetServerPaused
*PURPOSE:
*  Freeze or resume the in-process server tick. When paused
*  the hosted-server timer stays armed but skips its tick,
*  so the single-player world holds and resumes cleanly.
*  Only called for single-player; a listen-server host never
*  pauses so remote players keep simulating.
*********************************************************/
void gameFrontSetServerPaused(bool paused);

/*********************************************************
*NAME:          gameFrontGetServerSim
*PURPOSE:
*  Returns a pointer to the ServerSim for single-player,
*  or NULL if not in single-player mode.
*********************************************************/
ServerSim *gameFrontGetServerSim(void);

/*********************************************************
*NAME:          gameFrontGetPlayerNum
*PURPOSE:
*  Returns the local player's slot number.
*  0 for single-player, assigned by server for multiplayer.
*********************************************************/
BYTE gameFrontGetPlayerNum(void);

/*********************************************************
*NAME:          gameFrontGetSinglePlayerServerSim
*PURPOSE:
*  Returns the in-process spServerSim pointer for the
*  single-player lobby path, or NULL if no single-player
*  session is active. Used by the lobby UI to mutate
*  server-side state directly (settings, add bot, team
*  changes) instead of going through the UDP packet path.
*********************************************************/
struct ServerSim *gameFrontGetSinglePlayerServerSim(void);

/* Dialog window position — used to place main window on same monitor */
extern int gameFrontDialogX;
extern int gameFrontDialogY;

/* Lobby window size and players/map column split ([WINDOW] section).
 * The lobby reuses the dialog window, so its position is gameFrontDialogX/Y
 * above; -1 width/height means "never saved". The split offsets are in
 * logical (UI-scale-independent) pixels, one per right-panel view — the map
 * view uses gameFrontLobbySplit, the post-game replay uses
 * gameFrontLobbySplitRecap. Saved through the same debounced
 * gameFrontSaveWindowSettings path as the rest of the window state. */
extern int gameFrontLobbyW;
extern int gameFrontLobbyH;
extern float gameFrontLobbySplit;
extern float gameFrontLobbySplitRecap;

/* Map overview pop-out geometry and camera state ([WINDOW] section), plus
 * whether it was open when the last game ended ([MENU] section). The overview
 * is its own OS window, so unlike the lobby it needs a position of its own.
 * The zoom is stored as a camera scale rather than a ladder index so the
 * saved value keeps its meaning if the ladder changes. Written through the
 * debounced gameFrontSaveWindowSettings path as the player drags, resizes,
 * zooms and toggles follow. gameFrontShowMapOverview survives the hide the
 * end of a game triggers, which is what reopens the window with the next
 * one; an explicit close clears it. */
extern int   gameFrontOverviewW;        /* pop-out size, logical px */
extern int   gameFrontOverviewH;
extern int   gameFrontOverviewX;        /* -1 = never saved */
extern int   gameFrontOverviewY;
extern float gameFrontOverviewZoom;     /* camera scale, e.g. 1.0 */
extern bool  gameFrontOverviewFollow;
extern bool  gameFrontShowMapOverview;  /* open when the last game ended */

/* The size the scenario panel is held to, as a percent of the size the
 * game's own zoom gives it. The drag is clamped to this range and so is a
 * value read out of the preferences file, because a file somebody has edited
 * by hand must not be able to open the panel larger than the screen or so
 * small there is nothing left to get a pointer onto. */
#define SCN_PANEL_SCALE_MIN 25
#define SCN_PANEL_SCALE_MAX 300

/* Where the scenario panel sits, how big the player has made it and how
 * opaque it is drawn ([WINDOW] section). These four are the layout of the
 * panel the player touched last, and they are the fallback: a scenario the
 * player has never laid out opens its panel from them, so somebody who likes
 * the panel small and out of the way gets it that way the first time they
 * meet a new scenario. A scenario that has been laid out keeps its own four
 * numbers in a row of its own under [SCENARIO PANEL], written and read by
 * gameFrontSetScnPanelLayout / gameFrontGetScnPanelLayout below, and those
 * are what the panel is placed from while that scenario is the one on screen.
 *
 * The panel is an ImGui window
 * inside the main one rather than an OS window of its own, so the position is
 * a position in the main window's own render coordinates, not a desktop one.
 * -1 for either coordinate means the player has never moved it, and it opens
 * at the top-right of the game view; a position saved on a larger display is
 * clamped back inside the window it is restored into.
 *
 * The scale is a percent rather than a size in pixels so that the saved value
 * keeps its meaning at every zoom: 100 is the size the game's zoom alone
 * gives the panel, which is the size it had before it could be resized. -1
 * means it has never been resized, and a loaded value is clamped to
 * SCN_PANEL_SCALE_MIN..SCN_PANEL_SCALE_MAX, the same range the resize drag
 * is clamped to.
 *
 * The alpha is a percent of opaque, and it fades everything the panel puts
 * on screen: the black backing it is drawn on and every primitive the
 * scenario drew, together, so the panel stays one object at every setting.
 * It reaches no pixel that was not drawn on — the empty parts of the square
 * are as clear at 10 as they are at 100 — and it does not reach the border
 * or the corner grips, which are the frontend's own and have to stay
 * findable. It scales what the panel already looked like rather than setting
 * it outright, so 100 is the panel as it was before there was a slider — a
 * dim backing with the scenario's drawing at full strength on top — and that
 * is also what -1, never touched, resolves to. 0 leaves the square empty but
 * for those grips. A loaded value is clamped to 0..100.
 *
 * All four are written through the debounced gameFrontSaveWindowSettings
 * path, and only as a drag ends: a drag is a burst of values and only the one
 * it finishes on is worth a write. */
extern int   gameFrontScnPanelX;        /* -1 = never moved */
extern int   gameFrontScnPanelY;
extern int   gameFrontScnPanelScale;    /* percent, -1 = never resized */
extern int   gameFrontScnPanelAlpha;    /* percent, -1 = never set */

/* The longest scenario string the pair below is asked to key a row on. The
 * caller passes the scenario's file name where there is one, because two
 * scenarios can carry the same display name and the file name is what the
 * host actually loaded, so this is LOBBY_SCENARIO_FILE_LEN from
 * control_event.h and not the shorter name length. Both buffers are sized
 * from it and a longer string is truncated rather than refused. */
#define SCN_PANEL_SCENARIO_LEN 128

/* One scenario's own scenario-panel layout, kept under "SCENARIO PANEL" /
 * the scenario ("Survival.scenario.lua") as "x,y,scale,alpha", so each
 * scenario remembers where its overlay sat, how big it was and how opaque.
 * The four numbers mean exactly what the globals above mean, -1 and all, and
 * are clamped on the way out of the file the same way those are: the
 * preferences file is a text file a player can edit, and a bad row must not
 * open the panel off screen or too small to get a pointer onto.
 *
 * Get returns false, and leaves all four alone, when no row is remembered
 * yet or when the row does not read as four whole numbers — the caller keeps
 * the [WINDOW] fallback in that case. Set does nothing for a scenario with
 * no name, which is a game with no scenario behind it. */
bool gameFrontGetScnPanelLayout(const char *scenario, int *x, int *y,
                                int *scale, int *alpha);
void gameFrontSetScnPanelLayout(const char *scenario, int x, int y,
                                int scale, int alpha);

/* App full screen mode ([MENU] section). While it is on the main window is
 * full screen everywhere — menus, lobby and game — and every game opens in
 * the Full Screen Map view. That view is the map, so the pop-out above never
 * opens while this is on; the two are separate flags because the pop-out has
 * to be remembered across a spell of full screen and handed back on the way
 * out. It survives the automatic exit from the in-window map view at the end
 * of a game and is cleared only by the player turning full screen off. */
extern bool  gameFrontFullScreen;

extern bool gameFrontUseUpnp;
extern bool gameFrontUseNatTraversal;

/* Client-hosting settings ([HOSTING] section). Read once at startup by
 * gameFrontGetPrefs, applied to the server config in gameFrontSetupServer,
 * and persisted immediately by the per-setting write-through setters below
 * so both settings shells save identically without a close-time flush.
 * gameFrontHostingUploadPolicy holds an UploadPolicy value, and
 * gameFrontHostingScriptUploadPolicy a ScriptUploadPolicy value. */
extern unsigned short gameFrontHostingPort;            /* default 27500 */
extern bool           gameFrontHostingAllowSpec;       /* default Yes   */
extern bool           gameFrontHostingScripts;          /* default Yes   */
                              /* Run the script beside a hosted map. Off
                               * hosts the map plainly, so a player with a
                               * map pack can decline its script without
                               * deleting the file. Set on the scenario
                               * library before either attach site runs. */
extern int            gameFrontHostingScriptUploadPolicy; /* default ALLOW (0) */
                              /* What this host does with scripts players
                               * send it. Off refuses them and plays a map a
                               * player uploaded plainly, leaving every other
                               * map alone; Allow keeps them for the session;
                               * Persist keeps them in the directory below.
                               * Set on the scenario library beside the
                               * switch above as "not Off". */
extern char           gameFrontHostingScriptUploadDir[FILENAME_MAX];
                              /* Persist target dir for scripts; default
                               * <prefs path>uploads/Scripts */
extern int            gameFrontHostingScriptUploadMaxFiles;   /* 1-255, default 32 */
extern int            gameFrontHostingScriptUploadMaxStorage; /* MB, 1-4095, default 64 */
extern int            gameFrontHostingMaxSpec;         /* 1-32,  default 16 */
extern int            gameFrontHostingUploadPolicy;    /* default ALLOW (0) */
extern int            gameFrontHostingUploadMaxFiles;  /* 1-255, default 64 */
extern int            gameFrontHostingUploadMaxStorage;/* MB, 1-4095, default 8 */
extern char           gameFrontHostingUploadDir[FILENAME_MAX];
                              /* Persist target dir; default <prefs path>uploads */
extern bool           gameFrontHostingLogging;         /* default Yes   */
extern char           gameFrontHostingLogDir[FILENAME_MAX];
                              /* Round-log dir; default <prefs path> */
extern char           gameFrontHostingScenarioDir[FILENAME_MAX];
                              /* The scenarios this host offers on their own,
                               * independently of any map; default
                               * <prefs path>scenarios */
extern bool           gameFrontHostingServeReplays;    /* default Yes   */
                              /* Hand a finished round's log to players who
                               * ask for it. Yes leaves the serve policy at
                               * ROUND_LOG_SERVE_AUTO, which serves unless
                               * WinBolo.net is running; No forces it off. */
extern int            gameFrontHostingVoiceMode;       /* default ON (0) */
                              /* How the hosted server handles the voice its
                               * clients send it. Holds a ServerVoiceMode.
                               * serverVoiceProximity is not implemented and
                               * forwards the same as serverVoiceOn. */

void gameFrontSetHostingPort(unsigned short port);
void gameFrontSetHostingAllowSpec(bool allow);
void gameFrontSetHostingScripts(bool allow);
void gameFrontSetHostingMaxSpec(int maxSpec);
void gameFrontSetHostingUploadPolicy(int policy);
void gameFrontSetHostingUploadMaxFiles(int maxFiles);
void gameFrontSetHostingUploadMaxStorage(int maxStorageMb);
void gameFrontSetHostingUploadDir(const char *dir);
void gameFrontSetHostingScriptUploadPolicy(int policy);
void gameFrontSetHostingScriptUploadMaxFiles(int maxFiles);
void gameFrontSetHostingScriptUploadMaxStorage(int maxStorageMb);
void gameFrontSetHostingScriptUploadDir(const char *dir);
void gameFrontSetHostingLogging(bool logging);
void gameFrontSetHostingLogDir(const char *dir);
void gameFrontSetHostingScenarioDir(const char *dir);
void gameFrontSetHostingServeReplays(bool serve);
void gameFrontSetHostingVoiceMode(int mode);

/* Visibility rules a hosted game starts with ([GAME OPTIONS] section).
 * Read by gameFrontGetPrefs and pushed onto the sim with
 * serverSimSetViewPolicy by every path here that creates a server — the
 * listen server in gameFrontSetupServer and the single-player game in the
 * dialog state machine — because both open a lobby whose Visibility
 * dropdown has to start on what this player last chose. Each policy
 * global holds a ViewPolicy value; the decay globals hold seconds in the
 * VIEW_DECAY_MIN_SECS..VIEW_DECAY_MAX_SECS range. Each starts on the
 * rules an unconfigured server runs, so hosting with an untouched INI
 * leaves the sim as serverSimInit created it. */
extern int gameFrontViewPillPolicy;     /* VIEW_POLICY_STOCK_PILL */
extern int gameFrontViewBasePolicy;     /* VIEW_POLICY_STOCK_BASE */
extern int gameFrontViewAllyPolicy;     /* VIEW_POLICY_STOCK_ALLY */
extern int gameFrontViewPillDecaySecs;  /* 5-600, default 30 */
extern int gameFrontViewBaseDecaySecs;
extern int gameFrontViewAllyDecaySecs;
/* Classic mode, applied after the three policies above so it wins when
 * both are set: it forces pill Key, base Off and ally Off. Default off. */
extern bool gameFrontClassicMode;
/* Allied tanks standing in trees are sent to their allies. Applied before
 * classic mode, which forces it back off. Default off. */
extern bool gameFrontAlliesInTrees;
/* Which block of squares the map overview keeps live. Holds an
 * OverviewWindow; starts on OVERVIEW_WINDOW_STOCK. */
extern int gameFrontOverviewWindow;
/* What stops the player seeing inside that block. Holds a
 * LineOfSightMode; starts on LINE_OF_SIGHT_STOCK. Both are applied
 * before classic mode, which writes them itself. */
extern int gameFrontLineOfSight;

void gameFrontSetViewPillPolicy(int policy);
void gameFrontSetViewBasePolicy(int policy);
void gameFrontSetViewAllyPolicy(int policy);
void gameFrontSetViewPillDecaySecs(int secs);
void gameFrontSetViewBaseDecaySecs(int secs);
void gameFrontSetViewAllyDecaySecs(int secs);
void gameFrontSetClassicMode(bool on);
void gameFrontSetAlliesInTrees(bool on);
void gameFrontSetOverviewWindow(int window);
void gameFrontSetLineOfSight(int mode);

/* Which named visibility set the host last chose, and the hand-made set
 * to go back to when that choice is Custom. The globals above hold the
 * values a hosted game starts with; these two hold the choice behind
 * them, so picking a preset and picking Custom again lands the host back
 * where they were. gameFrontVisibilityCustomSaved is false until a custom
 * set has been written at least once — the lobby leaves its Custom row
 * blank rather than offering a set nobody made. */
extern int                gameFrontVisibilityPreset; /* VisibilityPreset */
extern VisibilitySettings gameFrontVisibilityCustom;
extern bool               gameFrontVisibilityCustomSaved;

void gameFrontSetVisibilityPreset(int preset);
void gameFrontSetVisibilityCustom(const VisibilitySettings *v);
/* The seven visibility globals above as one set. */
void gameFrontGetVisibilitySettings(VisibilitySettings *out);
/* Records a set as the host's choice: the seven globals, which named set
 * it is, and — when saveCustom is true and the set is none of the named
 * ones — the set itself. Call it wherever a host changes visibility,
 * rather than writing the three separately.
 *
 * saveCustom is false where the caller is only writing down what the
 * settings are on right now. Applying a preset sends its settings one at
 * a time, so the values pass through mixes that match no named set, and
 * every one of those would otherwise be saved as the hand-made set. The
 * lobby therefore passes false and writes the hand-made set itself, at
 * the points where the player edited or picked something. */
void gameFrontRememberVisibility(const VisibilitySettings *v, bool saveCustom);

/* Records a lobby pick as what the next game this machine hosts opens on:
 * single player, Internet New and LAN New alike. Each writes the global
 * and the [GAME OPTIONS] key straight away, so the pick survives a crash
 * as well as a clean quit. The lobby calls these only where the host
 * picked something on the machine running the server, and not in a
 * scenario lobby, whose type and bots are the scenario's. */
void gameFrontRememberGameType(gameType gt);
void gameFrontRememberAiPolicy(aiType ai);
void gameFrontRememberHiddenMines(bool hm);

#endif

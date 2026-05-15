/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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


/* Default keys — SDL_Scancode values (USB HID page 07) */
#define DEFAULT_FORWARD      8    /* SDL_SCANCODE_E */
#define DEFAULT_BACKWARD     7    /* SDL_SCANCODE_D */
#define DEFAULT_LEFT         22   /* SDL_SCANCODE_S */
#define DEFAULT_RIGHT        9    /* SDL_SCANCODE_F */
#define DEFAULT_SHOOT        44   /* SDL_SCANCODE_SPACE */
#define DEFAULT_LAY_MINE     225  /* SDL_SCANCODE_LSHIFT */

/* Scroll — numpad arrow cluster */
#define DEFAULT_SCROLLLEFT   92   /* SDL_SCANCODE_KP_4 */
#define DEFAULT_SCROLLUP     96   /* SDL_SCANCODE_KP_8 */
#define DEFAULT_SCROLLRIGHT  94   /* SDL_SCANCODE_KP_6 */
#define DEFAULT_SCROLLDOWN   93   /* SDL_SCANCODE_KP_5 */

/* View keys */
#define DEFAULT_TANKVIEW     23   /* SDL_SCANCODE_T */
#define DEFAULT_PILLVIEW     10   /* SDL_SCANCODE_G */
#define DEFAULT_ALLYVIEW     28   /* SDL_SCANCODE_Y */
#define DEFAULT_LGMVIEW      11   /* SDL_SCANCODE_H */
#define DEFAULT_BASEVIEW     16   /* SDL_SCANCODE_M */

/* Quick-build keys — number row 1-5 */
#define DEFAULT_QUICKTREE    30   /* SDL_SCANCODE_1 */
#define DEFAULT_QUICKROAD    31   /* SDL_SCANCODE_2 */
#define DEFAULT_QUICKWALL    32   /* SDL_SCANCODE_3 */
#define DEFAULT_QUICKPILLBOX 33   /* SDL_SCANCODE_4 */
#define DEFAULT_QUICKMINE    34   /* SDL_SCANCODE_5 */

/* Gunsight range — KP+ / Enter */
#define DEFAULT_SCROLL_GUNINCREASE  87   /* SDL_SCANCODE_KP_PLUS */
#define DEFAULT_SCROLL_GUNDECREASE  40   /* SDL_SCANCODE_RETURN */
#define DEFAULT_SCROLL_PILLVIEW     10   /* SDL_SCANCODE_G */

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
  openSkins,   /* Skins selection dialog */
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
  openLogViewer
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
*NAME:          gameFrontGetPassword
*AUTHOR:        John Morrison
*CREATION DATE: 24/2/99
*LAST MODIFIED: 24/2/99
*PURPOSE:
* The network module has tried to join a game with a
* password, request it here.
*
*ARGUMENTS:
* pword - Password slected
*********************************************************/
void gameFrontGetPassword(char *pword);

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
* Sets the AI type of the game. (From networking module)
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
*NAME:          gameFrontGetShowCountryFlagsInChat
*PURPOSE:
* Whether the chat / newswire / players panels should render the
* country flag next to a player's name. Defaults to TRUE; the WBN /
* Steam badges are not gated by this preference.
*********************************************************/
bool gameFrontGetShowCountryFlagsInChat(void);

/*********************************************************
*NAME:          gameFrontSetShowCountryFlagsInChat
*PURPOSE:
* Sets the country-flag-in-chat preference and persists it.
*********************************************************/
void gameFrontSetShowCountryFlagsInChat(bool show);

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

/*********************************************************
*NAME:          gameFrontLoadInBuiltMap
*AUTHOR:        John Morrison
*CREATION DATE: 1/5/00
*LAST MODIFIED: 1/5/00
*PURPOSE:
* Attempts to load the built in map by loading the
* compress resource. Returns Success
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontLoadInBuiltMap(void);

/*********************************************************
*NAME:          gameFrontLoadTutorial
*AUTHOR:        John Morrison
*CREATION DATE: 1/5/00
*LAST MODIFIED: 1/5/00
*PURPOSE:
* Attempts to load the built in tutorial by loading the
* compress resource. Returns Success
*
*ARGUMENTS:
*
*********************************************************/
bool gameFrontLoadTutorial(void);

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

/*********************************************************
*NAME:          gameFrontGetWinbolonetUse
*PURPOSE:
* Returns whether WinBolo.net is active (token exists).
*********************************************************/
bool gameFrontGetWinbolonetUse(void);

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
*NAME:          gameFrontGetServerSim
*PURPOSE:
*  Returns a pointer to the ServerSim for single-player,
*  or NULL if not in single-player mode.
*********************************************************/
ServerSim *gameFrontGetServerSim(void);

/*********************************************************
*NAME:          gameFrontIsServerHosted
*PURPOSE:
*  True when the local ServerSim is being driven by the
*  hosted-server timer thread (network host mode). False
*  for single-player, where the main thread drives the
*  sim. Callers use this to avoid double-ticking the sim
*  from the main thread when the timer thread already is.
*********************************************************/
bool gameFrontIsServerHosted(void);

/*********************************************************
*NAME:          gameFrontGetPlayerNum
*PURPOSE:
*  Returns the local player's slot number.
*  0 for single-player, assigned by server for multiplayer.
*********************************************************/
BYTE gameFrontGetPlayerNum(void);

/*********************************************************
*NAME:          gameFrontLoadDeferredMap
*PURPOSE:
*  Loads the map from the transport after lobby exit.
*  Called when the lobby dialog returns (game started)
*  and the map was downloaded in the background.
*RETURNS:
*  TRUE on success, FALSE on failure.
*********************************************************/
bool gameFrontLoadDeferredMap(struct ClientSim **cs);

/*********************************************************
*NAME:          gameFrontStartSinglePlayerGame
*PURPOSE:
*  Single-player path: transitions the local spServerSim
*  from serverStateLobby to serverStateRunning, applies
*  team alliances, creates tanks for connected players,
*  syncs an initial snapshot to the client, and flips
*  cs->inLobby/netStat so lobbyShow exits with result=1.
*  Called by the lobby UI's Start button when
*  cs->isSinglePlayer is true.
*RETURNS:
*  TRUE on success, FALSE if the local server isn't in a
*  state that can start (e.g. no players connected).
*********************************************************/
bool gameFrontStartSinglePlayerGame(struct ClientSim *cs);

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

extern bool gameFrontUseUpnp;
extern bool gameFrontUseNatTraversal;

#endif

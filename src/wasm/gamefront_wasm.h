/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * gamefront_wasm.h - How the browser client was launched
 *
 * The page URL is read once, at page start, into a WasmLaunch (main_wasm.c),
 * and the game-start code takes its mode from that instead of reading the
 * URL itself. The page's one-time setup and the per-game start are separate
 * calls. Included only by files under src/wasm.
 */

#ifndef GAMEFRONT_WASM_H
#define GAMEFRONT_WASM_H

#include "global.h"        /* MAP_STR_SIZE, PLAYER_NAME_LEN */
#include "../gui/input.h"  /* keyItems */

typedef enum {
  WASM_GAME_PRACTICE,   /* single player, opened in the in-game lobby */
  WASM_GAME_TUTORIAL,
  WASM_GAME_JOIN        /* ?game_key= / /join/<key>, or ?proxyURL= */
} WasmGameMode;

typedef struct {
  WasmGameMode mode;
  char gameKey[128];              /* ?game_key=, empty when absent */
  char devProxy[1024];            /* ?proxyURL=, empty when absent */
  char password[MAP_STR_SIZE];    /* ?password=, seeds the join password */
  char name[PLAYER_NAME_LEN];     /* ?name=, unvalidated */
  bool showMenu;                  /* true when the page should open on the
                                     menu rather than go straight into mode */
} WasmLaunch;

/* Set up what lasts for the life of the page, once, before any game: seed the
 * player name, the tracker and WinBolo.net token fields, the tank options and
 * the default keys, load the language, create the window on the page's canvas,
 * and bring up sound, the brains list and the message handler. Settings synced
 * or changed after this are never seeded again by a game start. Returns FALSE
 * if the window cannot be created. */
bool gameFrontWasmSetup(keyItems *keys);

/* Start the game the launch describes: join through the relay or dev proxy,
 * the guided tutorial, or single player. Resets the per-game state (game
 * options, password, server address, map file, tutorial step) first and
 * leaves the page-lifetime settings alone. Returns FALSE on an init failure
 * or a failed join; a failed join has already been reported through
 * wasmReportConnectFailure. */
bool gameFrontWasmStart(const char *cmdLine, keyItems *keys,
                        const WasmLaunch *launch);

#endif /* GAMEFRONT_WASM_H */

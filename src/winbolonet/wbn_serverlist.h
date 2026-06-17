/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          wbn_serverlist
*Filename:      wbn_serverlist.h
*Purpose:
*  Structs and a pure parser for the WinBolo.net public
*  game list (GET /api/v1/games), plus a thin fetch wrapper
*  that issues the unsigned GET and parses the response.
*********************************************************/

#ifndef __WBN_SERVERLIST_H
#define __WBN_SERVERLIST_H

#include "global.h"   /* MAX_TANKS, PLAYER_NAME_LEN */

#define WBN_MOTD_LINES     8     /* max MOTD lines kept; extras dropped */
#define WBN_MOTD_LINE_LEN  128   /* per-line width incl NUL; longer lines truncated */

typedef struct {
  char address[64];                          /* JSON "address" — join+ping target */
  int  port;                                 /* JSON "port" */
  char serverKey[64];                        /* "server_key" */
  char map[256];                             /* "map" */
  char mapMd5[33];                           /* "map_md5" */
  char version[32];                          /* "version" */
  char country[8];                           /* "country" — may be "" */
  int  gameType;                             /* "game_type" */
  int  ai;                                   /* "ai" (0..3) */
  bool mines;                                /* "mines" */
  bool password;                             /* "password" */
  bool randomMap;                            /* "random_map" */
  bool ranked;                               /* "ranked" */
  bool inLobby;                              /* "in_lobby" */
  bool hasLobby;                             /* "has_lobby" */
  bool allowNewPlayers;                      /* "allow_new_players" */
  bool autoLock;                             /* "auto_lock" */
  bool allowSpectators;                      /* "allow_spectators" (deferred; parsed, default false) */
  int  spectatorCount;                       /* "spectator_count" (deferred; default 0) */
  bool timeLimit;                            /* "time_limit" */
  int  timeMinutes;                          /* "time_minutes" */
  int  freeBases;                            /* "free_bases" */
  int  freePills;                            /* "free_pills" */
  int  numBases;                             /* "num_bases" — total bases */
  int  numPills;                             /* "num_pills" — total pills */
  int  numPlayers;                           /* "num_players" — in-game count incl bots */
  int  numHumans;                            /* "num_humans" */
  int  numBots;                              /* "num_bots" */
  int  maxPlayers;                           /* "max_players" */
  char players[MAX_TANKS][PLAYER_NAME_LEN];  /* "players" usernames, blanks filtered out */
  int  numPlayerNames;                       /* count of real entries in players[] */
} WbnServerListEntry;

typedef struct {
  int  tversion;
  char motd[WBN_MOTD_LINES][WBN_MOTD_LINE_LEN];
  int  numMotd;
  WbnServerListEntry *servers;   /* heap array, length `count`; NULL when count==0 */
  int  count;
} WbnServerList;

/*********************************************************
*NAME:          wbnServerListParse
*PURPOSE:
* Pure parser for a GET /api/v1/games response body. Zeroes
* *out, then fills it from the JSON. Does no network I/O.
* Returns true on success; on any failure (NULL/non-JSON
* input, a root that is not an object, or an absent/non-array
* "servers") *out is left zeroed and owning nothing, so a
* later wbnServerListFree is a no-op. Unknown fields and a
* tversion other than the expected value are tolerated
* (forward-compatible).
*
*ARGUMENTS:
* json - Response body string (may be NULL)
* out  - Receives the parsed list (caller frees with
*        wbnServerListFree)
*********************************************************/
bool wbnServerListParse(const char *json, WbnServerList *out);

/*********************************************************
*NAME:          wbnFetchServerList
*PURPOSE:
* Issues an unsigned GET /api/v1/games via wbn_api_get_public
* and parses the response into *out. Returns false on any
* non-2xx status (including 429) or a transport error, leaving
* *out zeroed; otherwise returns the parse result. No retry or
* Retry-After handling.
*
*ARGUMENTS:
* out - Receives the parsed list (caller frees with
*       wbnServerListFree)
*********************************************************/
bool wbnFetchServerList(WbnServerList *out);

/*********************************************************
*NAME:          wbnServerListFree
*PURPOSE:
* Frees the heap server array held by *out and resets it to
* empty. Safe to call when out is NULL or already zeroed. Does
* not free the fixed-size inline arrays.
*
*ARGUMENTS:
* out - List to free (may be NULL)
*********************************************************/
void wbnServerListFree(WbnServerList *out);

#endif /* __WBN_SERVERLIST_H */

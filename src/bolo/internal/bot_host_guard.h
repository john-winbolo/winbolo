/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot Host guard
 *Filename:      bot_host_guard.h
 *Purpose:
 *  The rules that keep a server from running code of its
 *  own choice on a player's computer through the Bot Host
 *  inside the player's client.
 *
 *    Official-only: a Bot Host runs only the brains that
 *      came with its install, checked file by file with
 *      SHA-256 (brainSyncInstallMatches). The server says
 *      "run this stock bot"; no brain file is sent. A
 *      player's built-in host is official-only when the
 *      client is built with WINBOLO_BOTHOST_OFFICIAL_ONLY
 *      (the default). Any host is official-only when the
 *      server runs with -bothost-code official (the
 *      default), which sets the manifest's noCode flag.
 *    Init tokens: on a player's official-only host a bot
 *      takes only the init tokens on a short list. cfg=
 *      (any GoalHunter constant) is never on it.
 *    Placement: a bot with an unculled view (any AI level
 *      but aiYes) runs on a player's computer only when
 *      that host said it takes one (BRAIN_PACK_CAP_FULL_MAP,
 *      sent by a build with WINBOLO_BOTHOST_ALLOW_FULL_MAP).
 *    Salts: a host answers few same-computer salts per
 *      connection, so a server cannot collect hashes.
 *    Recordings: a server takes a recording upload only
 *      for a block it opened to that host.
 *
 *  Pure functions, so the unit tests can check them.
 *********************************************************/

#ifndef BOT_HOST_GUARD_H
#define BOT_HOST_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scenario_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Init tokens ------------------------------------------------------- */

/* The longest token key or name the list takes. */
#define BOT_HOST_TOKEN_NAME_MAX 31

/* True when tok (one token, no ';' or ',') is on the list a player's
 * official-only host takes. Spaces in it are ignored, as the brain does.
 * The list (brains/GoalHunter/init.lua reads each):
 *   mode=<key> difficulty=<key> preset=<name>   [A-Za-z0-9_], 1..31
 *   survivor horde normal ammoless noammo suicider nosuicider noblitz
 *   blitzonly noclaimdead                         (bare words)
 *   portfolio=N/N/N[/N]                           each 0..100
 *   blitz=default  blitz=N[/N]                    each 1..16
 *   blitzsuiciders=N                              0..16
 *   refuel=X                                      0 < X <= 10, digits and '.'
 *   peace=N[/N...]                                each 0..15
 * Everything else is refused: cfg= and every other key. */
bool botHostInitTokenAllowed(const char *tok);

/* True when every token of arg (split on ';' and ',') is allowed. An empty
 * or NULL arg is allowed. */
bool botHostInitArgAllowed(const char *arg);

/* True when the init table t, made into tokens the way the brain does
 * (_flatten_init_table: a flag word "1", "" or "true" is the bare word, "0"
 * or "false" is nothing, anything else is key=value), has only allowed
 * tokens. A value with ';' or ',' in it is split as the brain splits it.
 * NULL or an empty table is allowed. */
bool botHostInitTableAllowed(const ScnTable *t);

/* ---- Placement --------------------------------------------------------- */

typedef struct {
    bool playerHost;    /* the host is a player's built-in host */
    bool officialOnly;  /* the server's -bothost-code official, or the host
                         * said it runs only its install's brains */
    bool brainInPlace;  /* the host's install holds the seat's brain, file
                         * for file (its pack request asked for nothing) */
    bool culled;        /* the seat's AI level is aiYes: it sees only what a
                         * player at its place sees */
    bool tokensAllowed; /* the seat's init arg and table pass the list */
    bool fullMapOk;     /* the host said it takes bots that see the whole
                         * map (BRAIN_PACK_CAP_FULL_MAP) */
} BotHostSeatFit;

/* True when the seat may run on the host. */
bool botHostSeatMayRunOn(const BotHostSeatFit *f);

/* True when a pack request (its file bitmap of bitsLen bytes, its file
 * count and the files it takes from the install) asks for no file and
 * takes every file from the install: the host runs the brain in place. */
bool botHostPackReqAllInstall(const uint8_t *bits, size_t bitsLen,
                              uint16_t fileCount, uint16_t fromInstall);

/* ---- Brain files from a server ----------------------------------------- */

/* The kinds of brain file data a server sends a host. */
#define BOT_HOST_SYNC_DATA_CHUNK 1  /* a piece of one file (one at a time) */
#define BOT_HOST_SYNC_DATA_PACK  2  /* a piece of a packed stream */

/* A Bot Host's brain sync state, as far as taking a server's file goes. */
typedef struct {
    bool     officialOnly; /* a player's host built with
                            * WINBOLO_BOTHOST_OFFICIAL_ONLY: it never takes
                            * a brain file from a server */
    bool     installOnly;  /* this manifest's brains run only from the
                            * install (official-only, or noCode) */
    bool     refused;      /* this manifest was refused (v1 or debug mode) */
    bool     haveManifest; /* a manifest is being downloaded */
    bool     ready;        /* every file of it is in place */
    bool     packMode;     /* its files come in packed streams */
    uint32_t gen;          /* its generation */
} BotHostSyncTake;

/* True when the host may take (check and write into its stage or cache
 * folder) brain file data of kind (BOT_HOST_SYNC_DATA_*) for manifest
 * generation gen. Never on an official-only host, nor for an install-only
 * or refused manifest: those hosts write no byte a server sends. */
bool botHostSyncTakesData(const BotHostSyncTake *s, int kind, uint32_t gen);

/* ---- Same-computer salts ----------------------------------------------- */

/* The most salts one connection is answered, and the least time between
 * two answers. A server sends one at the join, each lobby and each game
 * start: about two a game, never two in one second. */
#define BOT_HOST_SALT_MAX_PER_CONN 32
#define BOT_HOST_SALT_MIN_GAP_MS   1000u

typedef struct {
    int      answered;
    uint32_t lastMs;
} BotHostSaltLimit;

void botHostSaltLimitInit(BotHostSaltLimit *l);
/* True (and counted) when a salt at nowMs may be answered. */
bool botHostSaltLimitTake(BotHostSaltLimit *l, uint32_t nowMs);

/* ---- Download resets -------------------------------------------------- */

/* A host may reset its download (BRAIN_PACK_CAP_RESET) when its player
 * turns off "run bots on this computer". The reset clears the requests the
 * server took for the current list, so the host can ask again. Each
 * request loads files on the tick thread, so only the first few resets of
 * one list clear them. A new list clears the count. */
#define BOT_HOST_RESET_MAX_PER_LIST 4

typedef struct {
    int resets;   /* resets of the current list */
} BotHostResetLimit;

/* A new list went out: the count starts again. */
void botHostResetLimitInit(BotHostResetLimit *l);
/* Count one reset. True when it may clear the requests taken for the
 * list; false past BOT_HOST_RESET_MAX_PER_LIST resets of one list. */
bool botHostResetLimitTake(BotHostResetLimit *l);

/* ---- Recording uploads ------------------------------------------------- */

/* The blocks the server opened to its hosts, by host name and kind. Kept
 * for the whole run of the server, so a host that comes back (a new
 * connection) may still send a block it recorded. */
#define BOT_HOST_REC_ASKED_MAX   256
#define BOT_HOST_REC_ASKED_BLOCK 64
#define BOT_HOST_REC_ASKED_NAME  32

typedef struct {
    char    block[BOT_HOST_REC_ASKED_BLOCK];
    char    host[BOT_HOST_REC_ASKED_NAME];
    bool    playerHost;
} BotHostRecAskedEntry;

typedef struct {
    BotHostRecAskedEntry e[BOT_HOST_REC_ASKED_MAX];
    int count;
    int next;   /* the entry the next new one replaces when full */
} BotHostRecAsked;

void botHostRecAskedInit(BotHostRecAsked *a);
/* Note that block was opened to the host. */
void botHostRecAskedNote(BotHostRecAsked *a, const char *block,
                         const char *host, bool playerHost);
/* True when block was opened to a host of that name and kind. */
bool botHostRecAskedHas(const BotHostRecAsked *a, const char *block,
                        const char *host, bool playerHost);

#ifdef __cplusplus
}
#endif

#endif /* BOT_HOST_GUARD_H */

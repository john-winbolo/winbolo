/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The dedicated server's own Bot Host child.
 *
 * By default WinBoloDS runs its bots in a child WinBoloBotHost, with the
 * same OS sandbox, Lua sandbox and job limits as a player's child
 * (src/bothost_child/bothost_child.h). A brain crash or a stuck brain then
 * stops only the child, and the brains have no network and no file access
 * past the program folder and the child's cache folder.
 *
 * The child joins the server from this computer with a secret the server
 * made (JOIN_BOT_HOST_DS_CHILD_MARK). The server counts it as its own
 * process: lag 0, never refused by the same-computer rule, and the
 * server's own slots go to it. Each tick the server waits a short time for
 * the moves of the child's bots (transportUdpServerLockstepWait), and the
 * child's bots think once for each snapshot (--lockstep).
 *
 * When the child cannot run, the server runs its bots in its own process,
 * as before the child existed, and logs one line why:
 *   - -bots-in-process;
 *   - -brain-debug (the recording runs in the server's process);
 *   - -seed, -brain-lua-seed and -brain-no-budget-kill together (the
 *     recipe for a repeatable game: a game with the child is not
 *     repeatable, because the bots move to it at a tick that changes from
 *     run to run);
 *   - -allow-unsafe-brains (the child runs brains only in the Lua sandbox);
 *   - the child or its sandbox cannot start (no WinBoloBotHost, no
 *     Landlock on Linux, a failed start, a hello that does not match, the
 *     server refused it, or it did not join in time);
 *   - the child ended by itself more than BOT_HOST_CHILD_MAX_RESTARTS
 *     times in one game.
 * A player's game differs: it runs no bots then (botHostChildFailRule),
 * because the brain code is the server's, not the player's.
 *
 * The child's thread ("ds-bot-child") starts, watches and stops the child.
 * The server's tick never waits for it.
 */

#ifndef WINBOLO_DS_BOT_CHILD_H
#define WINBOLO_DS_BOT_CHILD_H

#include <stdbool.h>
#include <stdint.h>

#include "server_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        inProcess;     /* -bots-in-process */
    bool        brainDebug;    /* -brain-debug */
    bool        haveSeed;      /* -seed */
    bool        allowUnsafe;   /* -allow-unsafe-brains, -brain-profile-log */
    const char *brainPath;     /* -brain (a path to init.lua, or a folder) */
    int         threads;       /* -threads; 0: the default */
    bool        haveLuaSeed;   /* -brain-lua-seed */
    long        luaSeed;
    bool        noBudgetKill;  /* -brain-no-budget-kill */
    int         brainTier;     /* -brain-tier; 0: none */
    int         slots;         /* -botcapacity; 0: the child's capacity */
    const char *cacheDir;      /* -bothost-child-cache; NULL: the default */
    const char *serverAddr;    /* -addr; NULL: 127.0.0.1 */
    uint16_t    serverPort;
} DsBotChildConfig;

/* Starts the thread. With no bots to run in a child, it logs one line and
 * starts nothing. */
void dsBotChildStart(ServerSim *sim, const DsBotChildConfig *cfg);
/* Stops the thread and the child (it tells the child to quit). Call
 * before the tick timer stops. */
void dsBotChildStop(void);

#ifdef __cplusplus
}
#endif

#endif

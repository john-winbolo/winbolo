/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The dedicated server's own Bot Host child (ds_bot_child.h). */

#include "ds_bot_child.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "../bothost_child/bothost_child.h"
#include "bot_manager.h"     /* botManagerGameStarts */
#include "control_event.h"   /* BOT_HOST_TICKET_LEN */
#include "cpu_speed_store.h" /* cpuSpeedStorePath, for the child */
#include "threads.h"         /* threadsWaitForMutex */
#include "transport_udp.h"   /* transportUdpServerDsChild* */
#include "../common/wb_log.h"

/* A child that has not joined the server this long after its start is
 * stopped, and the bots run in the server's process. */
#define DS_CHILD_JOIN_WAIT_MS 30000u
/* How often the thread looks at the child. */
#define DS_CHILD_POLL_MS 20u
/* How often a start that waits for the sandbox's folders asks again. */
#define DS_CHILD_PREP_RETRY_MS 100u
/* After its last stop, the thread waits at most this long for a child that
 * was killed to end (botHostChildClientPoll reaps it). */
#define DS_CHILD_REAP_WAIT_MS 5000u

static SDL_Thread *dsThread;
static SDL_AtomicInt dsStop;
static ServerSim *dsSim;
static DsBotChildConfig dsCfg;
static char dsBrainName[64];
static char dsCache[1024];
static char dsAddr[64];
/* The child's log; "" when WINBOLO_BOTHOST_CHILD_LOG names one. */
static char dsLog[1100];

static void dsInProcess(const char *why) {
    fprintf(stderr, "[bothost] the server runs its bots in its own process "
                    "(%s)\n", why);
    WB_LOG_INFO(WB_LOG_CAT_NET, "[bothost] the server runs its bots in its "
                "own process (%s)", why);
}

/* "Brains/GoalHunter/init.lua" or "Brains/GoalHunter": "GoalHunter". */
static void dsBrainNameOf(const char *path, char *out, size_t outSize) {
    char buf[1024];
    size_t n;
    char *slash;
    out[0] = '\0';
    if (path == NULL || path[0] == '\0') return;
    SDL_strlcpy(buf, path, sizeof(buf));
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '/' || buf[n - 1] == '\\')) buf[--n] = '\0';
    if (n > 4 && SDL_strcasecmp(buf + n - 4, ".lua") == 0) {
        char *s1 = strrchr(buf, '/');
        char *s2 = strrchr(buf, '\\');
        char *cut = s1 > s2 ? s1 : s2;
        if (cut != NULL) {
            *cut = '\0';
        } else {
            buf[0] = '\0';
        }
    }
    {
        char *s1 = strrchr(buf, '/');
        char *s2 = strrchr(buf, '\\');
        slash = s1 > s2 ? s1 : s2;
    }
    SDL_strlcpy(out, slash != NULL ? slash + 1 : buf, outSize);
}

static void dsCacheDir(void) {
    char *pref;
    if (dsCfg.cacheDir != NULL && dsCfg.cacheDir[0] != '\0') {
        SDL_strlcpy(dsCache, dsCfg.cacheDir, sizeof(dsCache));
    } else {
        /* SDL_GetPrefPath ends in a separator. */
        pref = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (pref != NULL) {
            SDL_snprintf(dsCache, sizeof(dsCache), "%sbothost_ds_cache",
                         pref);
            SDL_free(pref);
        } else {
            SDL_strlcpy(dsCache, "bothost_ds_cache", sizeof(dsCache));
        }
    }
    (void)SDL_CreateDirectory(dsCache);
    /* Not the player's child's log: a game on this computer keeps its own.
     * The path goes in the start's parameters, not in the environment:
     * the server's threads run already. */
    if (SDL_getenv("WINBOLO_BOTHOST_CHILD_LOG") == NULL) {
        SDL_snprintf(dsLog, sizeof(dsLog), "%s/bothost-ds-child.log",
                     dsCache);
    } else {
        dsLog[0] = '\0';
    }
}

/* One start of the child, with the secret of this start attempt. */
static BotHostChildStart dsStartChild(const uint8_t *secret) {
    BotHostChildParams p;
    BotHostChildLimits lim;
    char speedFile[1024];

    memset(&p, 0, sizeof(p));
    p.serverHost = dsAddr;
    p.serverPort = dsCfg.serverPort;
    p.player = 0;
    p.brain = dsBrainName;
    p.name = "server child";
    p.cacheDir = dsCache;
    p.slots = dsCfg.slots;
    p.officialOnly = false;
    if (cpuSpeedStorePath(speedFile, sizeof(speedFile))) {
        p.cpuSpeedFile = speedFile;
    }
    p.dsChild = true;
    p.lockstep = true;
    p.threads = dsCfg.threads;
    p.haveLuaSeed = dsCfg.haveLuaSeed;
    p.luaSeed = dsCfg.luaSeed;
    p.noBudgetKill = dsCfg.noBudgetKill;
    p.brainTier = dsCfg.brainTier;
    p.logPath = dsLog[0] != '\0' ? dsLog : NULL;
    lim.memMb = BOT_HOST_CHILD_DEFAULT_MEM_MB;
    lim.cpuPct = BOT_HOST_CHILD_DEFAULT_CPU_PCT;
    return botHostChildClientStart(&p, secret, BOT_HOST_TICKET_LEN, &lim,
                                   botHostDsChildMax(), true);
}

/* Waits (on this thread, never the server's tick) for a child that was
 * killed and has not ended yet. */
static void dsReapWait(void) {
    uint32_t waited = 0;
    while (botHostChildClientReaping() && waited < DS_CHILD_REAP_WAIT_MS) {
        SDL_Delay(DS_CHILD_POLL_MS);
        waited += DS_CHILD_POLL_MS;
        botHostChildClientPoll();
    }
}

/* The child will not start again: the server's process takes its bots,
 * and the server no longer wants the child. */
static void dsGiveUp(const char *why) {
    threadsWaitForMutex();
    transportUdpServerDsChildEnd(dsSim, why);
    transportUdpServerDsChildGiveUp(dsSim);
    threadsReleaseMutex();
    dsReapWait();
}

static const char *dsEndWhy(BotHostChildEnd e) {
    switch (e) {
    case BOT_HOST_CHILD_ENDED_REFUSED:
        return "the server refused its Bot Host child";
    case BOT_HOST_CHILD_ENDED_MISMATCH:
        return "the Bot Host child does not match this server";
    default:
        return "the child or its sandbox cannot start";
    }
}

static int SDLCALL dsThreadFn(void *unused) {
    BotHostDsChildIn in;
    BotHostChildRestart restart;
    uint8_t secret[BOT_HOST_TICKET_LEN];
    uint32_t restartStarts = 0, startMs = 0;
    bool running = false, joined = false, haveStarts = false;
    bool haveSecret = false;
    const char *why = NULL;
    (void)unused;

    memset(&in, 0, sizeof(in));
    botHostChildRestartInit(&restart);
    while (!SDL_GetAtomicInt(&dsStop)) {
        uint32_t now = (uint32_t)SDL_GetTicks();
        if (!running) {
            BotHostDsWhere where;
            if (restart.waiting && !botHostChildRestartDue(&restart, now)) {
                SDL_Delay(DS_CHILD_POLL_MS);
                continue;
            }
            /* One secret for each start attempt: a start that waits for
             * the sandbox's folders asks again with the same one. */
            if (!haveSecret) {
                threadsWaitForMutex();
                haveSecret = transportUdpServerDsChildBegin(dsSim, secret);
                threadsReleaseMutex();
            }
            in.start = haveSecret ? dsStartChild(secret)
                                  : BOT_HOST_CHILD_START_INPROC;
            in.tried = true;
            where = botHostDsChildWhere(&in, &why);
            if (where == BOT_HOST_DS_WAIT) {
                SDL_Delay(DS_CHILD_PREP_RETRY_MS);
                continue;
            }
            /* The child has the secret now, or it will not start: the next
             * attempt makes a new one. */
            SDL_memset(secret, 0, sizeof(secret));
            haveSecret = false;
            if (where == BOT_HOST_DS_INPROC) {
                const char *detail = botHostChildClientFailWhy();
                char line[400];
                if (detail != NULL && detail[0] != '\0') {
                    SDL_snprintf(line, sizeof(line), "%s: %s", why, detail);
                } else {
                    SDL_strlcpy(line, why, sizeof(line));
                }
                botHostChildClientStop(false);
                dsGiveUp("the server's Bot Host child cannot start");
                dsInProcess(line);
                return 0;
            }
            running = true;
            joined = false;
            startMs = now;
            continue;
        }
        botHostChildClientPoll();
        {
            BotHostChildStatus st;
            botHostChildClientStatus(&st);
            if (st.connected && !joined) {
                joined = true;
                fprintf(stderr, "[bothost] the server's bots run in its Bot "
                                "Host child (sandbox on, lockstep wait %d "
                                "ms)\n",
                        transportUdpServerGetLockstepWaitMs());
            }
            if (st.running && !joined &&
                now - startMs >= DS_CHILD_JOIN_WAIT_MS) {
                botHostChildClientStop(false);
                dsGiveUp("the server's Bot Host child did not join");
                dsInProcess("the child did not join the server in 30 s");
                return 0;
            }
            if (!st.running) {
                BotHostChildEnd e = botHostChildClientEnd();
                uint32_t starts;
                botHostChildClientStop(false);
                running = false;
                if (e != BOT_HOST_CHILD_ENDED_CRASH) {
                    dsGiveUp("the server's Bot Host child ended");
                    dsInProcess(dsEndWhy(e));
                    return 0;
                }
                threadsWaitForMutex();
                transportUdpServerDsChildEnd(dsSim, "the server's Bot Host "
                                                    "child ended by itself");
                starts = botManagerGameStarts(dsSim);
                threadsReleaseMutex();
                if (!haveStarts || starts != restartStarts) {
                    /* A new game: its own restarts. */
                    botHostChildRestartInit(&restart);
                    restartStarts = starts;
                    haveStarts = true;
                }
                if (!botHostChildRestartAfterExit(&restart, now)) {
                    in.restartLimit = true;
                    (void)botHostDsChildWhere(&in, &why);
                    dsGiveUp("the server's Bot Host child ended by itself "
                             "too often");
                    dsInProcess(why);
                    return 0;
                }
                fprintf(stderr, "[bothost] the server's Bot Host child ended "
                                "by itself: it starts again in %u ms "
                                "(restart %d of %d this game)\n",
                        (unsigned)BOT_HOST_CHILD_RESTART_WAIT_MS,
                        restart.restarts, BOT_HOST_CHILD_MAX_RESTARTS);
                continue;
            }
        }
        SDL_Delay(DS_CHILD_POLL_MS);
    }
    if (running) botHostChildClientStop(true);
    SDL_memset(secret, 0, sizeof(secret));
    dsReapWait();
    return 0;
}

void dsBotChildStart(ServerSim *sim, const DsBotChildConfig *cfg) {
    BotHostDsChildIn in;
    const char *why = NULL;
    if (sim == NULL || cfg == NULL || dsThread != NULL) return;
    memset(&in, 0, sizeof(in));
    in.inProcessAsked = cfg->inProcess;
    in.brainDebug = cfg->brainDebug;
    in.repeatable = cfg->haveSeed && cfg->haveLuaSeed && cfg->noBudgetKill;
    in.allowUnsafe = cfg->allowUnsafe;
    if (botHostDsChildWhere(&in, &why) == BOT_HOST_DS_INPROC) {
        dsInProcess(why);
        return;
    }
    dsSim = sim;
    dsCfg = *cfg;
    dsBrainNameOf(cfg->brainPath, dsBrainName, sizeof(dsBrainName));
    if (cfg->serverAddr != NULL && cfg->serverAddr[0] != '\0' &&
        strcmp(cfg->serverAddr, "0.0.0.0") != 0) {
        SDL_strlcpy(dsAddr, cfg->serverAddr, sizeof(dsAddr));
    } else {
        SDL_strlcpy(dsAddr, "127.0.0.1", sizeof(dsAddr));
    }
    dsCfg.serverAddr = dsAddr;
    dsCfg.brainPath = NULL;
    dsCacheDir();
    dsCfg.cacheDir = dsCache;
    SDL_SetAtomicInt(&dsStop, 0);
    dsThread = SDL_CreateThread(dsThreadFn, "ds-bot-child", NULL);
    if (dsThread == NULL) {
        dsInProcess("the child's thread did not start");
    }
}

void dsBotChildStop(void) {
    if (dsThread == NULL) return;
    SDL_SetAtomicInt(&dsStop, 1);
    SDL_WaitThread(dsThread, NULL);
    dsThread = NULL;
}

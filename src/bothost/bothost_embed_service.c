/*
 * The clients' side of the Bot Host inside a WinBolo client: once a frame
 * the game and WinBoloHeadless call botHostEmbedService with the player's
 * ClientSim. It starts the host when the server sends this player a ticket
 * (CTRL_BOT_HOST_TICKET), stops it when the player leaves the game or a new
 * ticket replaces the old one, and tells the player on the newswire how
 * many of the server's bots this computer runs (unless the build sets
 * WINBOLO_HIDE_CLIENT_BOT_HOST_MSG).
 *
 * A ticket the server refused, or the ticket of a game the player left, is
 * not used again: the host starts again only with a new ticket, which the
 * server makes for each new connection (a rejoin).
 *
 * The player's Max slots (bothost_optout.h) is the host's own setting
 * (botHostEmbedSetMax); Max 0 offers the server 0 slots.
 *
 * The host runs as a child process (WinBoloBotHost --embed-child,
 * src/bothost_child/bothost_child.h) unless the settings say "in this
 * program", or the child cannot start: then it runs on a thread here, as
 * before, and one line in the log says so. The child gets the settings the
 * thread would use (official-only, the slots, -bothost-record, the brain
 * cache, the stored CPU speed). A child that ends by itself during a game
 * is started again at most BOT_HOST_CHILD_MAX_RESTARTS times for that
 * game; nothing is shown to the player. Meanwhile the server times the
 * lost host out and takes its bots back (botHostDrop, "timed out"), and a
 * host that joins again with the same ticket takes the old one's place.
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bothost_embed.h"
#include "bothost_optout.h"
#include "../bothost_child/bothost_child.h"
#include "../common/prefs.h"  /* botHostEmbedLoadPrefs */
#include "cpu_speed_store.h"  /* cpuSpeedStorePath, for the child */
#include "client_sim.h"     /* clientSimGetBotHostTicket, the server address */
#include "control_event.h"  /* BOT_HOST_TICKET_LEN, BOT_HOST_BRAIN_NAME_LEN */
#include "platform_types.h" /* BOLO_STATIC_ASSERT */
#include "../gui/lang.h"    /* langGetTextFmt, STR_CLIENT_BOTHOST_NEWS_FMT */

static BotHostEmbedSettings embedSettings = {
    false, 0, NULL, false, -1, -1
};
static char embedSettingsCache[1024];

/* The ticket the running host joined with, and the ClientSim it came on. */
static struct ClientSim *embedCs = NULL;
static uint8_t embedTicketNow[BOT_HOST_TICKET_LEN];
static bool embedHaveTicket = false;
/* A ticket not to use again: refused, or the player left that game. */
static uint8_t embedBlocked[BOT_HOST_TICKET_LEN];
static bool embedHaveBlocked = false;
/* The bot count last told on the newswire. */
static int embedLastBots = 0;
/* Where the running host is: a child process or a thread here. */
enum { EMBED_NONE, EMBED_THREAD, EMBED_CHILD };
static int embedWhere = EMBED_NONE;
/* The child could not run in this run (no program, a failed start, a
 * child that does not match): the host runs on a thread from now on. */
static bool embedChildOff = false;
/* A child of this run said hello once: a later child that ends before its
 * hello crashed; it is not a missing or old program. */
static bool embedChildHelloOnce = false;
/* The restarts of a child that ended by itself, for one ticket (a game). */
static BotHostChildRestart embedRestart;
static uint8_t embedRestartTicket[BOT_HOST_TICKET_LEN];
static bool embedHaveRestartTicket = false;
/* What the host is told, kept for a child started later. */
static int embedMaxNow = BOT_HOST_MAX_AUTO;
static bool embedRecordNow = false;
/* The last start found no sandbox: it said so in its own line. */
static bool embedStartNoBots = false;
/* The last start waits for the sandbox's folders: it tries again on a
 * later frame. */
static bool embedStartWait = false;

void botHostEmbedSettingsDefaults(BotHostEmbedSettings *settings) {
    if (settings == NULL) return;
    memset(settings, 0, sizeof(*settings));
    settings->childMemMb = -1;
    settings->childCpuPct = -1;
}

void botHostEmbedSetSettings(const BotHostEmbedSettings *settings) {
    botHostEmbedSettingsDefaults(&embedSettings);
    embedSettingsCache[0] = '\0';
    if (settings == NULL) return;
    embedSettings = *settings;
    if (settings->cacheDir != NULL) {
        SDL_strlcpy(embedSettingsCache, settings->cacheDir,
                    sizeof(embedSettingsCache));
        embedSettings.cacheDir = embedSettingsCache;
    }
}

static int prefsInt(const char *key, int def, int maxValue) {
    char buf[32], defText[32];
    long v;
    char *end = NULL;
    SDL_snprintf(defText, sizeof(defText), "%d", def);
    prefsGetString("BOT HOST", key, defText, buf, sizeof(buf));
    v = SDL_strtol(buf, &end, 10);
    if (end == buf || v < 0) return def;
    if (v > maxValue) v = maxValue;
    return (int)v;
}

void botHostEmbedLoadPrefs(void) {
    char buf[16];
    prefsGetString("BOT HOST", "Run As Child Process", "Yes", buf,
                   sizeof(buf));
    embedSettings.inProcess = SDL_strcasecmp(buf, "No") == 0 ||
                              SDL_strcmp(buf, "0") == 0;
    embedSettings.childMemMb =
        prefsInt("Child Memory Limit MiB", BOT_HOST_CHILD_DEFAULT_MEM_MB,
                 1024 * 1024);
    embedSettings.childCpuPct =
        prefsInt("Child CPU Limit Percent", BOT_HOST_CHILD_DEFAULT_CPU_PCT,
                 100);
    prefsGetString("BOT HOST", "Child File Sandbox", "Yes", buf, sizeof(buf));
    embedSettings.childNoSandbox = SDL_strcasecmp(buf, "No") == 0 ||
                                   SDL_strcmp(buf, "0") == 0;
    fprintf(stderr, "[bothost] this computer's Bot Host runs %s (memory "
                    "limit %d MiB, CPU cap %d%%, file sandbox %s)\n",
            embedSettings.inProcess ? "in this program"
                                    : "as a child process",
            embedSettings.childMemMb, embedSettings.childCpuPct,
            embedSettings.childNoSandbox ? "off" : "on");
}

/* ---- Where the host runs: a child process or a thread here ----------- */

static void embedChildLimits(BotHostChildLimits *lim) {
    lim->memMb = embedSettings.childMemMb < 0 ? BOT_HOST_CHILD_DEFAULT_MEM_MB
                                              : embedSettings.childMemMb;
    lim->cpuPct = embedSettings.childCpuPct < 0
                      ? BOT_HOST_CHILD_DEFAULT_CPU_PCT
                      : embedSettings.childCpuPct;
}

static BotHostChildStart embedStartChild(const BotHostEmbedConfig *cfg) {
    BotHostChildParams p;
    BotHostChildLimits lim;
    char speedFile[1024];

    memset(&p, 0, sizeof(p));
    p.serverHost = cfg->serverHost;
    p.serverPort = cfg->serverPort;
    p.player = cfg->player;
    p.brain = cfg->brain;
    p.name = cfg->name;
    p.cacheDir = cfg->cacheDir;
    p.slots = cfg->slots;
    p.record = embedRecordNow;
    p.recordMinFreeGb = cfg->recordMinFreeGb;
    p.officialOnly = botHostEmbedOfficialOnly();
    p.shippedTable = botHostEmbedShippedTable();
    if (cpuSpeedStorePath(speedFile, sizeof(speedFile))) {
        p.cpuSpeedFile = speedFile;
    }
    embedChildLimits(&lim);
    return botHostChildClientStart(&p, cfg->ticket, BOT_HOST_TICKET_LEN,
                                   &lim, embedMaxNow,
                                   !embedSettings.childNoSandbox);
}

bool botHostEmbedStart(const BotHostEmbedConfig *cfg) {
    if (cfg == NULL || cfg->ticket == NULL) return false;
    if (embedWhere != EMBED_NONE) botHostEmbedStop(true);
    embedStartNoBots = false;
    embedStartWait = false;
    if (!embedSettings.inProcess && !embedChildOff) {
        BotHostChildStart how = embedStartChild(cfg);
        if (how == BOT_HOST_CHILD_STARTED) {
            embedWhere = EMBED_CHILD;
            return true;
        }
        if (how == BOT_HOST_CHILD_START_WAIT) {
            /* The sandbox's folders are made ready on a thread. */
            embedStartWait = true;
            return false;
        }
        if (how == BOT_HOST_CHILD_START_NO_BOTS) {
            /* No sandbox: no bots for this game, and no thread here in
             * its place (that would run the brains with no OS sandbox).
             * botHostChildClientStart said why, in one line. */
            embedStartNoBots = true;
            return false;
        }
        /* botHostChildClientStart said why, in one line. */
        embedChildOff = true;
    }
    if (!botHostInprocStart(cfg)) return false;
    embedWhere = EMBED_THREAD;
    return true;
}

void botHostEmbedStop(bool tellServer) {
    if (embedWhere == EMBED_CHILD) {
        botHostChildClientStop(tellServer);
    } else if (embedWhere == EMBED_THREAD) {
        botHostInprocStop(tellServer);
    }
    embedWhere = EMBED_NONE;
}

bool botHostEmbedRunning(void) {
    if (embedWhere == EMBED_CHILD) return botHostChildClientActive();
    if (embedWhere == EMBED_THREAD) return botHostInprocRunning();
    return false;
}

void botHostEmbedGetStatus(BotHostEmbedStatus *out) {
    BotHostChildStatus cst;
    if (out == NULL) return;
    if (embedWhere != EMBED_CHILD) {
        botHostInprocGetStatus(out);
        return;
    }
    botHostChildClientPoll();
    if (botHostChildClientHello()) embedChildHelloOnce = true;
    botHostChildClientStatus(&cst);
    memset(out, 0, sizeof(*out));
    out->running = cst.running;
    out->connected = cst.connected;
    out->refused = cst.refused;
    out->slots = cst.slots;
    out->bots = cst.bots;
    out->sameServer = cst.sameServer;
    out->consentAsking = cst.consentAsking;
}

void botHostEmbedSetMax(int max) {
    if (max < 0) max = BOT_HOST_MAX_AUTO;
    if (max > BOT_HOST_MAX_TOP) max = BOT_HOST_MAX_TOP;
    embedMaxNow = max;
    botHostInprocSetMax(max);
    if (embedWhere == EMBED_CHILD) botHostChildClientSetMax(max);
}

void botHostEmbedConsentAnswer(bool yes) {
    if (embedWhere == EMBED_CHILD) {
        BotHostEmbedStatus st;
        botHostEmbedGetStatus(&st);
        if (!st.consentAsking) return;
        fprintf(stderr, "[bothost] the player said %s to the server's own "
                        "bot code for this join\n", yes ? "yes" : "no");
        botHostChildClientConsent(yes);
    } else {
        botHostInprocConsentAnswer(yes);
    }
}

void botHostEmbedSetRecord(bool on) {
    embedRecordNow = on;
    botHostInprocSetRecord(on);
}

static void embedStopNow(bool tellServer, const char *why) {
    if (!botHostEmbedRunning()) return;
    fprintf(stderr, "[bothost] this computer's Bot Host stops (%s)\n", why);
    botHostEmbedStop(tellServer);
    embedLastBots = 0;
}

static void embedBlock(const uint8_t *ticket) {
    memcpy(embedBlocked, ticket, sizeof(embedBlocked));
    embedHaveBlocked = true;
}

static void embedCacheDir(char *out, size_t outSize) {
    char *pref;
    if (embedSettings.cacheDir != NULL && embedSettings.cacheDir[0] != '\0') {
        SDL_strlcpy(out, embedSettings.cacheDir, outSize);
        return;
    }
    /* SDL_GetPrefPath ends in a separator. */
    pref = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (pref != NULL) {
        SDL_snprintf(out, outSize, "%sbothost_cache", pref);
        SDL_free(pref);
    } else {
        SDL_strlcpy(out, "bothost_cache", outSize);
    }
}

static void embedStartFor(struct ClientSim *cs, uint8_t player,
                          const uint8_t *ticket, const char *brain) {
    BotHostEmbedConfig cfg;
    struct in_addr a = clientSimGetServerAddress(cs);
    const uint8_t *ab = (const uint8_t *)&a;
    char host[32], name[64], cache[1024];

    SDL_snprintf(host, sizeof(host), "%u.%u.%u.%u", (unsigned)ab[0],
                 (unsigned)ab[1], (unsigned)ab[2], (unsigned)ab[3]);
    name[0] = '\0';
    clientSimGetPlayerName(cs, name, sizeof(name));
    embedCacheDir(cache, sizeof(cache));

    memset(&cfg, 0, sizeof(cfg));
    cfg.serverHost = host;
    cfg.serverPort = clientSimGetServerPort(cs);
    cfg.player = player;
    cfg.ticket = ticket;
    cfg.brain = brain;
    cfg.name = name;
    cfg.cacheDir = cache;
    cfg.slots = embedSettings.slots;

    embedCs = cs;
    memcpy(embedTicketNow, ticket, sizeof(embedTicketNow));
    embedHaveTicket = true;
    embedLastBots = 0;
    if (!embedHaveRestartTicket ||
        memcmp(ticket, embedRestartTicket, sizeof(embedRestartTicket)) != 0) {
        /* A new game: its own restarts. */
        botHostChildRestartInit(&embedRestart);
        memcpy(embedRestartTicket, ticket, sizeof(embedRestartTicket));
        embedHaveRestartTicket = true;
    }
    if (!botHostEmbedStart(&cfg)) {
        if (embedStartWait) {
            /* Not blocked: the next frame asks again. */
            embedHaveTicket = false;
            return;
        }
        if (!embedStartNoBots) {
            fprintf(stderr, "[bothost] this computer's Bot Host did not "
                            "start\n");
        }
        embedBlock(ticket);
        embedHaveTicket = false;
    }
}

/* The child ended by itself (its status says it does not run). True when
 * this took care of it; false for a refusal, which the caller handles as
 * it does for the thread. */
static bool embedChildEnded(struct ClientSim *cs, uint8_t player,
                            const uint8_t *ticket, const char *brain) {
    BotHostChildEnd end = botHostChildClientEnd();
    if (end == BOT_HOST_CHILD_RUNS || end == BOT_HOST_CHILD_ENDED_REFUSED) {
        return false;
    }
    if (end == BOT_HOST_CHILD_ENDED_SANDBOX ||
        ((end == BOT_HOST_CHILD_ENDED_MISMATCH ||
          (end == BOT_HOST_CHILD_ENDED_EARLY && !embedChildHelloOnce)) &&
         botHostChildFailRule(end == BOT_HOST_CHILD_ENDED_MISMATCH
                                  ? BOT_HOST_CHILD_FAIL_MISMATCH
                                  : BOT_HOST_CHILD_FAIL_NO_HELLO,
                              botHostChildClientSandboxed()) ==
             BOT_HOST_CHILD_START_NO_BOTS)) {
        /* The child said why in its log; the game said it in one line. No
         * restart and no thread here: no bots for this game. */
        embedStopNow(false, "the child could not run in its sandbox");
        embedBlock(embedTicketNow);
        embedHaveTicket = false;
        return true;
    }
    if (end == BOT_HOST_CHILD_ENDED_MISMATCH ||
        (end == BOT_HOST_CHILD_ENDED_EARLY && !embedChildHelloOnce)) {
        /* Not a crash: the child program cannot do this job. The host
         * runs on a thread here from now on. */
        fprintf(stderr, "[bothost] this computer's Bot Host child could not "
                        "run: the Bot Host runs in this program\n");
        embedStopNow(false, "the child could not run");
        embedChildOff = true;
        embedStartFor(cs, player, ticket, brain);
        return true;
    }
    /* A crash, a brain the limits stopped, or a kill. */
    embedStopNow(false, "the child ended by itself");
    embedHaveTicket = false;
    if (botHostChildRestartAfterExit(&embedRestart,
                                     (uint32_t)SDL_GetTicks())) {
        fprintf(stderr, "[bothost] this computer's Bot Host child ended by "
                        "itself; it starts again in %u ms\n",
                (unsigned)BOT_HOST_CHILD_RESTART_WAIT_MS);
    } else {
        fprintf(stderr, "[bothost] this computer's Bot Host child ended by "
                        "itself after %d restarts: no Bot Host on this "
                        "computer for the rest of this game\n",
                BOT_HOST_CHILD_MAX_RESTARTS);
        embedBlock(embedTicketNow);
    }
    return true;
}

void botHostEmbedService(struct ClientSim *cs) {
    uint8_t player = 0;
    uint8_t ticket[BOT_HOST_TICKET_LEN];
    char brain[BOT_HOST_BRAIN_NAME_LEN];
    bool haveTicket;
    BotHostEmbedStatus st;

    if (cs == NULL) {
        embedStopNow(true, "the player left the game");
        embedRestart.waiting = false;
        embedHaveTicket = false;
        embedCs = NULL;
        return;
    }
    if (embedSettings.disabled) return;
    {
        /* Once a run: session folders a crash left. */
        static bool swept = false;
        if (!swept) {
            char cache[1024];
            swept = true;
            embedCacheDir(cache, sizeof(cache));
            botHostEmbedSweepSessions(cache);
        }
    }

    haveTicket = clientSimGetBotHostTicket(cs, &player, ticket, brain, NULL);
    if (botHostEmbedRunning()) {
        if (cs != embedCs || !haveTicket ||
            memcmp(ticket, embedTicketNow, sizeof(ticket)) != 0) {
            embedStopNow(true, haveTicket ? "a new ticket came"
                                          : "the player left the game");
            embedHaveTicket = false;
        } else {
            botHostEmbedGetStatus(&st);
            if (!st.running && embedWhere == EMBED_CHILD &&
                embedChildEnded(cs, player, ticket, brain)) {
                return;
            }
            if (!st.running) {
                /* The thread ended by itself: the server refused it. */
                embedStopNow(true, "the server refused it");
                embedBlock(embedTicketNow);
                embedHaveTicket = false;
                return;
            }
#ifndef WINBOLO_HIDE_CLIENT_BOT_HOST_MSG
            if (st.connected && st.bots != embedLastBots) {
                if (st.bots > 0) {
                    MessageArgs args;
                    memset(&args, 0, sizeof(args));
                    args.number = st.bots;
                    clientSimNewswireText(
                        cs, langGetTextFmt(STR_CLIENT_BOTHOST_NEWS_FMT, &args));
                }
                embedLastBots = st.bots;
            }
#endif
            return;
        }
    }
    if (!haveTicket) {
        embedRestart.waiting = false;
        return;
    }
    if (embedHaveBlocked &&
        memcmp(ticket, embedBlocked, sizeof(ticket)) == 0) {
        return;
    }
    if (embedRestart.waiting) {
        /* A child that ended by itself starts again after a short wait,
         * for the same game only. */
        if (cs != embedCs || !embedHaveRestartTicket ||
            memcmp(ticket, embedRestartTicket, sizeof(ticket)) != 0) {
            embedRestart.waiting = false;
        } else if (!botHostChildRestartDue(&embedRestart,
                                           (uint32_t)SDL_GetTicks())) {
            return;
        } else {
            fprintf(stderr, "[bothost] this computer's Bot Host child starts "
                            "again (restart %d of %d for this game)\n",
                    embedRestart.restarts, BOT_HOST_CHILD_MAX_RESTARTS);
        }
    }
    embedStartFor(cs, player, ticket, brain);
}

void botHostEmbedLeave(const char *why) {
    if (embedRestart.waiting && embedHaveRestartTicket) {
        /* The player left while a child waited to start again. */
        embedBlock(embedRestartTicket);
    }
    embedRestart.waiting = false;
    if (embedHaveTicket) embedBlock(embedTicketNow);
    embedStopNow(true, why != NULL ? why : "the player left the game");
    embedHaveTicket = false;
    embedCs = NULL;
}

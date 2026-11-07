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
 */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bothost_embed.h"
#include "client_sim.h"     /* clientSimGetBotHostTicket, the server address */
#include "control_event.h"  /* BOT_HOST_TICKET_LEN, BOT_HOST_BRAIN_NAME_LEN */
#include "../gui/lang.h"    /* langGetTextFmt, STR_CLIENT_BOTHOST_NEWS_FMT */

static BotHostEmbedSettings embedSettings;
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

void botHostEmbedSetSettings(const BotHostEmbedSettings *settings) {
    memset(&embedSettings, 0, sizeof(embedSettings));
    embedSettingsCache[0] = '\0';
    if (settings == NULL) return;
    embedSettings = *settings;
    if (settings->cacheDir != NULL) {
        SDL_strlcpy(embedSettingsCache, settings->cacheDir,
                    sizeof(embedSettingsCache));
        embedSettings.cacheDir = embedSettingsCache;
    }
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
    if (!botHostEmbedStart(&cfg)) {
        fprintf(stderr, "[bothost] this computer's Bot Host did not start\n");
        embedBlock(ticket);
        embedHaveTicket = false;
    }
}

void botHostEmbedService(struct ClientSim *cs) {
    uint8_t player = 0;
    uint8_t ticket[BOT_HOST_TICKET_LEN];
    char brain[BOT_HOST_BRAIN_NAME_LEN];
    bool haveTicket;
    BotHostEmbedStatus st;

    if (cs == NULL) {
        embedStopNow(true, "the player left the game");
        embedHaveTicket = false;
        embedCs = NULL;
        return;
    }
    if (embedSettings.disabled) return;

    haveTicket = clientSimGetBotHostTicket(cs, &player, ticket, brain, NULL);
    if (botHostEmbedRunning()) {
        if (cs != embedCs || !haveTicket ||
            memcmp(ticket, embedTicketNow, sizeof(ticket)) != 0) {
            embedStopNow(true, haveTicket ? "a new ticket came"
                                          : "the player left the game");
            embedHaveTicket = false;
        } else {
            botHostEmbedGetStatus(&st);
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
    if (!haveTicket) return;
    if (embedHaveBlocked &&
        memcmp(ticket, embedBlocked, sizeof(ticket)) == 0) {
        return;
    }
    embedStartFor(cs, player, ticket, brain);
}

void botHostEmbedLeave(const char *why) {
    if (embedHaveTicket) embedBlock(embedTicketNow);
    embedStopNow(true, why != NULL ? why : "the player left the game");
    embedHaveTicket = false;
    embedCs = NULL;
}

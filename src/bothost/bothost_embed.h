/*
 * The Bot Host inside a WinBolo client.
 *
 * Each player's client (the game and WinBoloHeadless) runs a Bot Host of
 * its own while it is in a game on a dedicated server, so the server can
 * move its bots' brains onto the players' computers. The host is the
 * WinBoloBotHost program's code (bothost_main.c, built a second time by
 * bothost_embed.c), run on a thread of its own inside the client. It serves
 * the one server the player is on, opens no listen port, and joins with the
 * ticket the server sent the player (CTRL_BOT_HOST_TICKET) in place of a key.
 * The server takes it only while the player is connected and drops it, with
 * every bot on it going back to the server, when the player leaves.
 *
 * botHostEmbedService (bothost_embed_service.c) is what the clients call
 * once a frame: it starts the host when a ticket comes and stops it when
 * the player leaves.
 */
#ifndef BOTHOST_EMBED_H
#define BOTHOST_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

typedef struct {
    const char    *serverHost;   /* the server's address, as the player joined */
    uint16_t       serverPort;
    uint8_t        player;       /* the player's number */
    const uint8_t *ticket;       /* BOT_HOST_TICKET_LEN bytes */
    const char    *brain;        /* the server's bot brain, for its capacity */
    const char    *name;         /* the host's name: the player's name */
    const char    *cacheDir;     /* where the server's brain files are kept */
    int            slots;        /* > 0: offer this many; 0: the capacity */
    double         recordMinFreeGb; /* 0: the default (50) */
} BotHostEmbedConfig;

typedef struct {
    bool running;    /* the host's thread runs */
    bool connected;  /* the server took the host */
    bool refused;    /* the server refused it */
    int  slots;      /* the bots it offers to run */
    int  bots;       /* the bots it runs now */
    bool sameServer; /* the server is on this computer: the host is
                      * stopped for this join (CTRL_BOT_HOST_SAME_SERVER) */
} BotHostEmbedStatus;

/* What the client's System Info shows of the host (botHostEmbedGetInfo).
 * The host's thread writes a copy of these numbers under a lock after each
 * of its ticks; the reader takes that copy under the same lock. */
enum {
    BOT_HOST_EMBED_STATE_RUNNING = 0,  /* connected and holds every brain file */
    BOT_HOST_EMBED_STATE_SYNCING,      /* connected, brain files still coming */
    BOT_HOST_EMBED_STATE_SAME_SERVER,  /* stopped: the server is on this computer */
    BOT_HOST_EMBED_STATE_OFF,          /* the player turned bots off for this game */
    BOT_HOST_EMBED_STATE_CONNECTING    /* not connected to the server yet */
};

#define BOT_HOST_EMBED_INFO_BOTS     16
#define BOT_HOST_EMBED_INFO_NAME_LEN 32

typedef struct {
    char     name[BOT_HOST_EMBED_INFO_NAME_LEN]; /* the bot's player name */
    double   lastMs;     /* its last think */
    double   peakMs;     /* its worst think since it came to this host */
    uint32_t overruns;
    uint32_t thinks;
} BotHostEmbedBotInfo;

typedef struct {
    bool     running;      /* the host's thread runs; nothing else is set
                            * when it does not */
    int      state;        /* BOT_HOST_EMBED_STATE_* */
    int      syncPercent;  /* SYNCING: 0..99 */
    int      bots;         /* bots running here */
    int      slots;        /* slots offered to the server now */
    /* The overrun rate over the last windowSecs seconds, by the server's
     * own rule (botHostOverrunRate): known only when bots thought in each
     * of those seconds. */
    int      windowSecs;
    bool     rateKnown;
    double   ratePct;
    /* One bot's part of this computer's CPU in thousandths of a percent
     * (bot_host_cpu.h): measured, else the full-budget value (cpuAtMost),
     * else BOT_HOST_CPU_MILLI_UNKNOWN. */
    uint32_t cpuBotMilli;
    bool     cpuAtMost;
    uint8_t  freePct;      /* this computer's free CPU, 0..100; 0xFF not known */
    int      workers;      /* bot pool worker threads; 0 is a single thread */
    double   targetMs;     /* the think target for one bot */
    int      botCount;     /* entries used in bot[] */
    BotHostEmbedBotInfo bot[BOT_HOST_EMBED_INFO_BOTS];
} BotHostEmbedInfo;

/* The state from what the host knows: the server is on this computer, the
 * player turned bots off for this game, the control connection is up, and
 * the brain files are all here. */
static inline int botHostEmbedStateOf(bool sameServer, bool withheld,
                                      bool connected, bool synced) {
    if (sameServer) return BOT_HOST_EMBED_STATE_SAME_SERVER;
    if (withheld) return BOT_HOST_EMBED_STATE_OFF;
    if (!connected) return BOT_HOST_EMBED_STATE_CONNECTING;
    if (!synced) return BOT_HOST_EMBED_STATE_SYNCING;
    return BOT_HOST_EMBED_STATE_RUNNING;
}

/* The brain sync percent for done of total bytes: 0..99 until it is done,
 * so "syncing" never reads 100%. */
static inline int botHostEmbedSyncPercent(uint32_t done, uint32_t total) {
    uint64_t pct;
    if (total == 0) return 0;
    pct = (uint64_t)done * 100u / total;
    return pct > 99 ? 99 : (int)pct;
}

/* The CPU per bot the block shows: the measured value when there is one,
 * else the full-budget value marked "at most", else unknown (0xFFFFFFFF,
 * BOT_HOST_CPU_MILLI_UNKNOWN). */
static inline uint32_t botHostEmbedCpuPick(uint32_t measured, uint32_t budget,
                                           bool *atMost) {
    if (atMost != NULL) *atMost = false;
    if (measured != 0xFFFFFFFFu) return measured;
    if (budget != 0xFFFFFFFFu) {
        if (atMost != NULL) *atMost = true;
        return budget;
    }
    return 0xFFFFFFFFu;
}

/* A copy of the host's numbers; running is false when no host runs. Main
 * thread. */
void botHostEmbedGetInfo(BotHostEmbedInfo *out);

/* Start the host (a running one stops first). Main thread only. */
bool botHostEmbedStart(const BotHostEmbedConfig *cfg);
/* Stop the host and wait for its thread. tellServer: each bot connection
 * and the host itself send their quit, so the server takes the bots back at
 * once; false only for a program that ends next (a crash test). */
void botHostEmbedStop(bool tellServer);
bool botHostEmbedRunning(void);
/* Remove the brain session folders under cacheDir that an earlier run left
 * (marked, and no running client holds their lock). */
void botHostEmbedSweepSessions(const char *cacheDir);
void botHostEmbedGetStatus(BotHostEmbedStatus *out);
/* on: the host offers the server 0 slots (CTRL_BOT_HOST_SLOTS), so the
 * server takes its bots back, and it takes no new seat; it stays connected.
 * off: it offers its slots again. It holds across a start and a stop; the
 * clients' side (botHostEmbedService) sets it each frame from the lobby's
 * one-game opt-out. Main thread only. */
void botHostEmbedSetWithhold(bool on);
/* The slots offered before the brain's own answer is known. */
int botHostEmbedDefaultSlots(void);

/* ---- The clients' side (bothost_embed_service.c) ---------------------- */

typedef struct {
    bool        disabled;    /* never host (a test option) */
    int         slots;       /* > 0: a fixed slot count (a test option) */
    const char *cacheDir;    /* NULL: the default under the prefs folder */
} BotHostEmbedSettings;

void botHostEmbedSetSettings(const BotHostEmbedSettings *settings);

/* Once a frame on the main thread, with the player's ClientSim of a
 * network game, or NULL when there is none (the menu, a local game). It
 * starts the host when the server's ticket comes and stops it when the
 * player leaves the game, or when a new ticket replaces the old one. */
void botHostEmbedService(struct ClientSim *cs);

/* Stop at once: the player quits the game. */
void botHostEmbedLeave(const char *why);

/* The lobby's one-game opt-out: "Don't run bots on this computer for this
 * game". It is never saved. It goes off again when the player is in
 * another game: a new connection (a new ticket), another ClientSim, or no
 * game at all. botHostEmbedService sends it to the host each frame. */

/* True while the server lets this computer run bots in the game of cs
 * (its ticket came, hosting is not off, the server did not refuse the
 * host, and the server is not on this computer): the lobby shows the
 * opt-out only then. */
bool botHostEmbedOffered(struct ClientSim *cs);
/* Turn the opt-out on or off for the game of cs. */
void botHostEmbedSetOptOut(struct ClientSim *cs, bool on);
/* The opt-out for the game of cs; false for any other game. */
bool botHostEmbedOptOut(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_EMBED_H */

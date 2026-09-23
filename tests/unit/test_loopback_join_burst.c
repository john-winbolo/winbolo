/*
 * A second player joining a lobby full of bots, over the real loopback
 * transport.
 *
 * A player joining a lobby is sent the whole lobby state at once: the join
 * replay (serverSimSyncSubscriber) delivers the phase, settings, script list,
 * rules, brain list, brain texts and bot-name catalogue, then a slot record,
 * bot config, bot brain and player join for every seat. All of it goes onto
 * the joiner's reliable control channel inside one server tick, before the
 * joiner has had the chance to ack any of it. When that burst is larger than
 * CHANNEL_CONTROL_WINDOW the channel refuses the rest, and the server reads
 * the refusal as a client that stopped acking and drops the joiner.
 *
 * Each case seats its bots through CMD_LOBBY_ADD_BOT from the host, the way
 * the lobby's Add Bot button does, one bot every few pumps so the host's own
 * channel is never the one that fills.
 *
 * Fourteen bots alone bring the replay to about one window, so the large case
 * also installs a bot-name catalogue of generated names, which is what a
 * server started with a large -botnames file sends. Its chunk count is
 * measured with the same serialize call the replay uses. Before the second
 * player joins, the case works out the size of the replay from the lobby's
 * state and requires at least 1.5 windows, so a brain more or less, or a
 * catalogue chunk more or less, does not take it back under the window.
 *
 * Failure messages start "sizing:" when the lobby never reached the burst the
 * case needs, and "join:" when the second player's join went wrong.
 *
 * The overflow is seen from the server's deferred-disconnect flag, read after
 * every pump, and from the second client itself: whether it is still
 * connected, whether its map download finished, and whether its lobby holds a
 * record for every seated bot.
 *
 *   run_loopback_join_lobby_burst   14 bots and a large catalogue
 *   run_loopback_join_small_lobby   two bots, well inside one window
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_ADD_BOT, CmdResult */
#include "client_sim.h"
#include "client_sim_internal.h"   /* lobbySyncSettled */
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "control_event.h"         /* LOBBY_BOT_POOL_CHUNK_FRAG_MAX */
#include "channel_mux.h"           /* CHANNEL_CONTROL_WINDOW */
#include "lobby_bot_pools.h"
#include "players.h"               /* playersIsInUse */
#include "server_sim.h"
#include "server_sim_internal.h"   /* playerConnected, lobbyPlayers,
                                    * serverSimScriptListChunkCount */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "transport_udp.h"         /* transportUdpServerTestPendingRemove */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define JB_CONNECT_MAX 2000   /* the host's join and map download         */
#define JB_SETTLE_MAX   600   /* the host's lobby sync settling            */
#define JB_ADD_PUMPS     10   /* pumps between two Add Bots                */
#define JB_JOIN_MAX    3000   /* the second player's join, download, sync  */
#define JB_AFTER         50   /* pumps watched once the joiner looks done  */

/* Every bot seat the lobby has once both humans are in it. */
#define JB_MAX_BOTS (MAX_TANKS - 2)

/* The generated catalogue: 4 pools of 230 names, 60 letters each, is about
 * 56 KB uncompressed, under LOBBY_BOT_CATALOG_MAX_BYTES (64 KiB). */
#define JB_POOLS      4
#define JB_POOL_NAMES 230
#define JB_NAME_LEN   60

/* The fewest catalogue chunks the large case accepts. */
#define JB_MIN_CHUNKS 32

typedef struct {
    const char *label;
    int         bots;
    bool        bigCatalogue;
    int         minBurst;   /* 0 = no floor */
    uint64_t    seed;
} JbCase;

static char jbBrainPath[512];

static bool jbMakeBrainFile(void) {
    FILE *f;

    if (!utScratchPath(jbBrainPath, sizeof(jbBrainPath),
                       "join_burst_brain.lua")) {
        return false;
    }
    f = fopen(jbBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

/* ── The generated catalogue ──────────────────────────────────────────
 *
 * Letters from a seeded xorshift, both cases, so zlib finds little to
 * squeeze: about 5.7 bits of information in each 8-bit byte. Its own
 * generator rather than bolo_rand, so building it draws nothing from the
 * stream the harness seeds. */
static char *jbNameStore;
static const char **jbNamePtrs;

static uint32_t jbXorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static bool jbInstallCatalogue(void) {
    static const char letters[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static const char *labels[JB_POOLS] = {
        "Burst One", "Burst Two", "Burst Three", "Burst Four"
    };
    LobbyBotPoolDef defs[JB_POOLS];
    LobbyBotPoolLoadStats st;
    uint32_t s = 0x9E3779B9u;
    int total = JB_POOLS * JB_POOL_NAMES;
    int i, c, p;

    jbNameStore = (char *)malloc((size_t)total * (JB_NAME_LEN + 1));
    jbNamePtrs  = (const char **)malloc((size_t)total * sizeof(*jbNamePtrs));
    if (jbNameStore == NULL || jbNamePtrs == NULL) return false;

    for (i = 0; i < total; i++) {
        char *nm = jbNameStore + (size_t)i * (JB_NAME_LEN + 1);
        for (c = 0; c < JB_NAME_LEN; c++) {
            nm[c] = letters[jbXorshift(&s) % (sizeof(letters) - 1)];
        }
        nm[JB_NAME_LEN] = '\0';
        jbNamePtrs[i] = nm;
    }
    for (p = 0; p < JB_POOLS; p++) {
        defs[p].label     = labels[p];
        defs[p].names     = jbNamePtrs + p * JB_POOL_NAMES;
        defs[p].nameCount = JB_POOL_NAMES;
    }
    memset(&st, 0, sizeof(st));
    return lobbyBotPoolsInstall(defs, JB_POOLS, &st) == JB_POOLS &&
           st.namesKept == total;
}

/* How many CTRL_LOBBY_BOT_POOL_CHUNK events the replay sends for the active
 * catalogue: the same serialize call and the same fragment size the replay
 * uses. -1 if it could not be serialized. */
static int jbCatalogueChunks(void) {
    unsigned char *blob = (unsigned char *)malloc(LOBBY_BOT_CATALOG_WIRE_MAX);
    int blen;

    if (blob == NULL) return -1;
    blen = lobbyBotPoolsSerialize(blob, (int)LOBBY_BOT_CATALOG_WIRE_MAX);
    free(blob);
    if (blen < 0) return -1;
    return (blen + LOBBY_BOT_POOL_CHUNK_FRAG_MAX - 1) /
           LOBBY_BOT_POOL_CHUNK_FRAG_MAX;
}

/* The fewest control events a joiner's replay carries, worked out from the
 * lobby's state along serverSimSyncSubscriber's lobby path:
 *
 *   phase, settings, rules, brain list, closing marker   5
 *   script list                                          1 per chunk
 *   bot-name catalogue                                   1 per chunk
 *   lobby slot                                           1 per seat
 *   bot config + bot brain                               2 per bot
 *   player join                                          1 per player
 *
 * Brain texts, team metadata, votes and the rest are left out: each only
 * adds, and how much depends on what the machine has installed. The joiner's
 * own slot and join are left out too, because it is not seated yet. */
static int jbExpectedBurst(ServerSim *sim, int catalogueChunks) {
    int n = 5;
    int i;

    n += (int)serverSimScriptListChunkCount(sim);
    n += catalogueChunks;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) {
            n += 1;
            if (sim->lobbyPlayers[i].isBot) n += 2;
        }
        if (playersIsInUse(&sim->sim.plyrs, (BYTE)i)) n += 1;
    }
    return n;
}

static bool jbHostConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool jbHostSettled(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs->lobbySyncSettled;
}

/* The deferred-disconnect flag is cleared by the removal drain later in the
 * same server tick that set it, so it is sampled after every pump and any
 * sighting is kept. */
static bool jbSawPendingRemove;

static void jbPump(LoopbackHarness *h) {
    int s;
    loopbackHarnessPump(h);
    for (s = 0; s < MAX_TANKS; s++) {
        if (transportUdpServerTestPendingRemove(s)) {
            jbSawPendingRemove = true;
        }
    }
}

/* The second client is done joining: it holds the map, and the replay's
 * closing marker has been applied. */
static bool jbJoinerDone(LoopbackHarness *h) {
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED &&
           h->cs2->lobbySyncSettled;
}

/* Seat `want` bots through the host's Add Bot. Returns how many were
 * seated. */
static int jbAddBots(LoopbackHarness *h, BYTE host, int want) {
    int added = 0;
    int i;

    while (added < want) {
        ClientCommand cmd;
        CmdResult r;

        memset(&cmd, 0, sizeof(cmd));
        cmd.type = CMD_LOBBY_ADD_BOT;
        cmd.cmdSeq = (uint32_t)(added + 1);
        threadsWaitForMutex();
        r = serverSimApplyCommand(h->sim, (int)host, &cmd);
        threadsReleaseMutex();
        if (r != CMD_OK) {
            fprintf(stderr, "  join burst: Add Bot %d refused (%d)\n",
                    added + 1, (int)r);
            break;
        }
        added++;
        for (i = 0; i < JB_ADD_PUMPS; i++) {
            jbPump(h);
        }
    }
    return added;
}

/* The flow, with every early return left to jbRun's cleanup. */
static int jbFlow(LoopbackHarness *h, const JbCase *tc) {
    BYTE host;
    BYTE joiner;
    int seated;
    int chunks;
    int burst;
    int at;
    int i;
    int botRecords;
    int serverBots;

    UT_ASSERT_MSG(loopbackHarnessStart(h, "Host", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, tc->seed),
                  "sizing: %s: loopback start failed", tc->label);

    at = loopbackHarnessPumpUntil(h, JB_CONNECT_MAX, jbHostConnected, NULL);
    UT_ASSERT_MSG(at > 0, "sizing: %s: the host never connected", tc->label);
    at = loopbackHarnessPumpUntil(h, JB_SETTLE_MAX, jbHostSettled, NULL);
    UT_ASSERT_MSG(at > 0, "sizing: %s: the host's lobby sync never settled",
                  tc->label);
    host = clientSimGetMyPlayerNum(h->cs);
    UT_ASSERT_MSG(host < MAX_TANKS, "sizing: %s: the host holds no slot",
                  tc->label);

    threadsWaitForMutex();
    serverSimSetBotAiType(h->sim, aiFull);
    serverSimSetBotBrainPath(h->sim, jbBrainPath);
    threadsReleaseMutex();

    seated = jbAddBots(h, host, tc->bots);
    UT_ASSERT_MSG(seated == tc->bots, "sizing: %s: seated %d of %d bots",
                  tc->label, seated, tc->bots);
    UT_ASSERT_MSG(!jbSawPendingRemove,
                  "sizing: %s: a control overflow fired while the bots were "
                  "being seated, before the second player joined", tc->label);

    /* The catalogue goes in after the bots, so the host, which has already
       had its replay, is never sent it; only the joiner's replay carries it. */
    if (tc->bigCatalogue) {
        UT_ASSERT_MSG(jbInstallCatalogue(),
                      "sizing: %s: the generated catalogue did not install "
                      "whole", tc->label);
    }
    chunks = jbCatalogueChunks();
    UT_ASSERT_MSG(chunks >= 0, "sizing: %s: the catalogue did not serialize",
                  tc->label);
    if (tc->bigCatalogue) {
        UT_ASSERT_MSG(chunks >= JB_MIN_CHUNKS,
                      "sizing: %s: the catalogue is %d chunks, under the %d "
                      "this case needs", tc->label, chunks, JB_MIN_CHUNKS);
    }

    threadsWaitForMutex();
    burst = jbExpectedBurst(h->sim, chunks);
    threadsReleaseMutex();
    fprintf(stderr, "  %s: %d bots, %d catalogue chunks, join replay at least "
                    "%d control events (window %d)\n",
            tc->label, seated, chunks, burst, CHANNEL_CONTROL_WINDOW);
    UT_ASSERT_MSG(burst >= tc->minBurst,
                  "sizing: %s: the join replay is at least %d control events, "
                  "under the %d this case needs", tc->label, burst,
                  tc->minBurst);

    /* The join. */
    UT_ASSERT_MSG(loopbackHarnessAddClient(h, "Joiner"),
                  "join: %s: the second client could not start connecting",
                  tc->label);
    for (i = 0; i < JB_JOIN_MAX && !jbJoinerDone(h); i++) {
        ClientConnectState st;
        jbPump(h);
        st = clientSimGetConnectState(h->cs2);
        if (st != CLIENT_CONNECT_JOINING &&
            st != CLIENT_CONNECT_DOWNLOADING_MAP &&
            st != CLIENT_CONNECT_CONNECTED) {
            break;   /* dropped: no point pumping on */
        }
    }
    for (i = 0; i < JB_AFTER; i++) {
        jbPump(h);
    }

    UT_ASSERT_MSG(!jbSawPendingRemove,
                  "join: %s: the server flagged a client for a deferred "
                  "disconnect during the join (control channel overflow)",
                  tc->label);
    UT_ASSERT_MSG(clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED,
                  "join: %s: the second client is in connect state %d, not "
                  "connected", tc->label,
                  (int)clientSimGetConnectState(h->cs2));
    joiner = clientSimGetMyPlayerNum(h->cs2);
    UT_ASSERT_MSG(joiner < MAX_TANKS && joiner != host,
                  "join: %s: the second client holds slot %u", tc->label,
                  (unsigned)joiner);
    UT_ASSERT_MSG(h->sim->playerConnected[joiner],
                  "join: %s: the server no longer has the second client in "
                  "slot %u", tc->label, (unsigned)joiner);
    UT_ASSERT_MSG(transportUdpServerTestDownloadComplete(joiner),
                  "join: %s: the server never saw the second client's map "
                  "download complete", tc->label);
    UT_ASSERT_MSG(h->cs2->lobbySyncSettled,
                  "join: %s: the second client never applied the end of the "
                  "join replay", tc->label);

    botRecords = 0;
    serverBots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *ls;
        if (!h->sim->playerConnected[i] || !h->sim->lobbyPlayers[i].isBot) {
            continue;
        }
        serverBots++;
        ls = clientSimGetLobbySlot(h->cs2, (BYTE)i);
        if (ls != NULL && ls->connected && ls->isBot) {
            botRecords++;
        }
    }
    UT_ASSERT_MSG(serverBots == seated,
                  "join: %s: the server has %d bots, %d were seated",
                  tc->label, serverBots, seated);
    UT_ASSERT_MSG(botRecords == seated,
                  "join: %s: the second client holds lobby records for %d of "
                  "the %d seated bots", tc->label, botRecords, seated);
    return 0;
}

static int jbRun(const JbCase *tc) {
    LoopbackHarness h;
    int rc;

    UT_ASSERT(jbMakeBrainFile());
    ut_brain_stub_arm(true);
    jbSawPendingRemove = false;
    memset(&h, 0, sizeof(h));

    rc = jbFlow(&h, tc);

    /* Every path: the harness down, the built-in catalogue back (a fresh
       unit process runs on the built-in one; the clients here install
       whatever the server sends into the same process-global table), and
       the fixture gone. */
    loopbackHarnessStop(&h);
    lobbyBotPoolsReset();
    free(jbNamePtrs);
    free(jbNameStore);
    jbNamePtrs  = NULL;
    jbNameStore = NULL;
    remove(jbBrainPath);
    return rc;
}

int run_loopback_join_lobby_burst(void) {
    static const JbCase tc = {
        "loopback_join_lobby_burst", JB_MAX_BOTS, true,
        CHANNEL_CONTROL_WINDOW + CHANNEL_CONTROL_WINDOW / 2, 0xB0257u
    };
    return jbRun(&tc);
}

int run_loopback_join_small_lobby(void) {
    static const JbCase tc = {
        "loopback_join_small_lobby", 2, false, 0, 0x5A11u
    };
    return jbRun(&tc);
}

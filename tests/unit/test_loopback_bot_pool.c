/*
 * Copyright (c) 1998-2026 John Morrison.
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

/*
 * The bot-name catalogue leaves the join burst.
 *
 * A joiner used to be sent the server's whole catalogue in its lobby sync, up
 * to 78 control chunks, whether or not it already held the same pools. Now
 * the sync carries one CTRL_LOBBY_BOT_POOL_INFO naming the catalogue by id,
 * and a client whose own pools have another id fetches the blob on
 * CHANNEL_BULK (PACKET_LOBBY_BOT_POOL_REQ, BULK_KIND_BOT_POOL).
 *
 *   lobby_bot_pool_join_sends_only_the_id — a registering subscriber gets one
 *       info event naming the server's catalogue and no chunks, and the
 *       spectator ring's control snapshot gets neither.
 *
 *   loopback_bot_pool_fetch — over the real UDP transport under loss, a
 *       client holding other pools than the server's fetches the server's
 *       and installs them.
 *
 *   loopback_bot_pool_same_not_fetched — a client already holding the
 *       server's pools, the usual case, asks for nothing.
 *
 *   bot_pool_refresh_retakes_catalogue — a sim made before other pools are
 *       loaded keeps its first copy until serverSimRefreshBotPools, and then
 *       names and hands out the new pools. WinBoloDS depends on this: it
 *       makes the sim before it loads -botnames or the shipped file.
 *
 *   loopback_bot_pool_survives_bulk_rebase — the server re-bases the
 *       client's bulk channel while the catalogue answer is part way in; the
 *       client asks again at once, not after a timeout, and installs the
 *       server's pools. The server sends the answer over several ticks, so
 *       stepping the client and the server separately and re-basing as soon
 *       as the client has read part of it cuts the rest off without relying
 *       on loss. The path is clean for that reason: a lost request would
 *       also end in a timeout and make the test fail for the wrong cause.
 *       If the answer is never seen part way in, the test fails rather than
 *       passing without having re-based anything.
 *
 * In the loopback cases the server and the client share this process, and
 * with it the process-global pool table. The server takes its copy of the
 * catalogue when the sim is made; the fetch case then installs other pools
 * in the table, which is the client's view, and has the server name its own
 * catalogue again.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "everard_map.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* lobbyPoolState */
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "control_event.h"
#include "lobby_bot_pools.h"
#include "log_internal.h"          /* serverSimSerializeControlSnapshot,
                                    * LOG_CONTROL_SNAPSHOT_MAX */
#include "server_sim.h"
#include "server_sim_internal.h"   /* botPoolId / botPoolBlobLen */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_lifecycle.h"      /* serverInstanceTick */
#include "transport_udp.h"         /* transportUdpClientTestBotPoolArriving */
#include "transport_udp_server_internal.h" /* serverRebaseBulkAndRearmDownload */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX  2000
#define SETTLE_MAX   2000
#define FETCH_MAX    4000   /* re-asks + bulk resends under loss */

/* ── The join sync ──────────────────────────────────────────────────── */

typedef struct {
    int      infos;
    int      chunks;
    uint32_t id;
    uint32_t len;
} BpSink;

static void bpDeliver(void *ctx, const ControlEvent *evt) {
    BpSink *s = (BpSink *)ctx;
    if (evt->type == CTRL_LOBBY_BOT_POOL_CHUNK) s->chunks++;
    if (evt->type != CTRL_LOBBY_BOT_POOL_INFO) return;
    s->infos++;
    s->id  = evt->u.lobbyBotPoolInfo.id;
    s->len = evt->u.lobbyBotPoolInfo.len;
}

int run_lobby_bot_pool_join_sends_only_the_id(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim;
    BpSink     sink;
    SubscriberHandle h;
    uint8_t   *snap;
    int        snapLen, pos;
    int        infoRecords = 0, chunkRecords = 0;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    lobbyBotPoolsReset();
    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);

    UT_ASSERT_MSG(sim->botPoolId != 0 && sim->botPoolBlobLen > 0,
                  "the server holds no catalogue of the built-in pools");
    UT_ASSERT_MSG(sim->botPoolId == lobbyBotPoolsCatalogId(),
                  "the server's catalogue id is not the id of the pools it "
                  "was made from");

    memset(&sink, 0, sizeof(sink));
    h = serverSimRegisterSubscriber(sim, bpDeliver, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    serverSimUnregisterSubscriber(sim, h);
    UT_ASSERT_MSG(sink.chunks == 0,
                  "the joiner was sent %d retired catalogue chunk(s)",
                  sink.chunks);
    UT_ASSERT_MSG(sink.infos == 1, "the joiner was sent %d catalogue info "
                  "events, expected 1", sink.infos);
    UT_ASSERT(sink.id == sim->botPoolId && sink.len == sim->botPoolBlobLen);

    /* The ring's snapshot carries neither. */
    snap = (uint8_t *)malloc(LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT(snap != NULL);
    snapLen = serverSimSerializeControlSnapshot(sim, snap,
                                                LOG_CONTROL_SNAPSHOT_MAX);
    UT_ASSERT(snapLen >= 0);
    for (pos = 0; pos + 4 <= snapLen;) {
        uint16_t type    = (uint16_t)((snap[pos] << 8) | snap[pos + 1]);
        uint16_t bodyLen = (uint16_t)((snap[pos + 2] << 8) | snap[pos + 3]);
        if (type == (uint16_t)CTRL_LOBBY_BOT_POOL_INFO) infoRecords++;
        if (type == (uint16_t)CTRL_LOBBY_BOT_POOL_CHUNK) chunkRecords++;
        pos += 4 + bodyLen;
    }
    UT_ASSERT_MSG(pos == snapLen, "the snapshot's framing did not add up");
    UT_ASSERT_MSG(infoRecords == 0 && chunkRecords == 0,
                  "the control snapshot carries %d info and %d chunk "
                  "records for the catalogue", infoRecords, chunkRecords);

    free(snap);
    serverSimDestroy(sim);
    threadsDestroy();
    return 0;
}

/* ── Over the wire ──────────────────────────────────────────────────── */

static bool pred_connected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_settled(LoopbackHarness *h, void *u) {
    (void)u;
    return h->cs->lobbySyncSettled;
}

static bool pred_pool_settled(LoopbackHarness *h, void *u) {
    (void)u;
    return h->cs->lobbyPoolState == CLIENT_BOT_POOL_S_HAVE ||
           h->cs->lobbyPoolState == CLIENT_BOT_POOL_S_FAILED;
}

static void publishCb(ServerSim *sim, const ControlEvent *evt) {
    serverSimPublishControl(sim, evt);
}

/* Pools other than the built-in ones, as a client that last joined another
 * server might hold. */
static bool installOtherPools(void) {
    static const char *names[] = { "Alpha", "Bravo", "Charlie", "Delta" };
    LobbyBotPoolDef def;
    def.label     = "Other Server";
    def.names     = names;
    def.nameCount = 4;
    return lobbyBotPoolsInstall(&def, 1, NULL) == 1;
}

int run_loopback_bot_pool_fetch(void) {
    LoopbackHarness h;
    ControlEvent    evt;
    uint32_t        serverId;
    char            label0[64];
    int             at;
    uint8_t         st;

    lobbyBotPoolsReset();
    SDL_strlcpy(label0, lobbyBotPoolLabel(0), sizeof(label0));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "PoolReader", /*lobbyMode*/ true,
                                       "loss=5,burst=2", /*seed*/ 0xB0A7u),
                  "harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    serverId = h.sim->botPoolId;
    UT_ASSERT(serverId != 0);

    /* The client's table changes under it; the server keeps its copy. */
    UT_ASSERT(installOtherPools());
    UT_ASSERT(lobbyBotPoolsCatalogId() != serverId);

    /* The join already told the client the server's id, when its table was
       still the server's, so it settled on HAVE. Forget that, so the wait
       below is for the answer to the info sent now. */
    h.cs->lobbyPoolState = CLIENT_BOT_POOL_S_NONE;
    threadsWaitForMutex();
    serverSimFillBotPoolInfoEvent(h.sim, &evt);
    publishCb(h.sim, &evt);
    threadsReleaseMutex();

    at = loopbackHarnessPumpUntil(&h, FETCH_MAX, pred_pool_settled, NULL);
    st = h.cs->lobbyPoolState;
    fprintf(stderr, "  bot pool (loss=5,burst=2): settled@%d state=%u "
                    "tries=%u\n", at, (unsigned)st,
            (unsigned)h.cs->lobbyPoolTries);
    if (at < 0 || st != CLIENT_BOT_POOL_S_HAVE) {
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("the server's catalogue did not arrive within %d pumps "
                "(state %u)", FETCH_MAX, (unsigned)st);
    }
    UT_ASSERT_MSG(h.cs->lobbyPoolTries >= 1,
                  "the catalogue settled without being asked for");
    UT_ASSERT_MSG(lobbyBotPoolsCatalogId() == serverId,
                  "the client installed a catalogue whose id is %08x, the "
                  "server's is %08x", (unsigned)lobbyBotPoolsCatalogId(),
                  (unsigned)serverId);
    UT_ASSERT_MSG(strcmp(lobbyBotPoolLabel(0), label0) == 0,
                  "pool 0 reads '%s' after the fetch, the server's is '%s'",
                  lobbyBotPoolLabel(0), label0);

    loopbackHarnessStop(&h);
    lobbyBotPoolsReset();
    return 0;
}

int run_loopback_bot_pool_same_not_fetched(void) {
    LoopbackHarness h;
    int             at;
    uint8_t         st, tries;

    lobbyBotPoolsReset();
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "PoolHolder", /*lobbyMode*/ true,
                                       NULL, /*seed*/ 0xB0A8u),
                  "harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at >= 0) {
        at = loopbackHarnessPumpUntil(&h, SETTLE_MAX, pred_settled, NULL);
    }
    st    = h.cs->lobbyPoolState;
    tries = h.cs->lobbyPoolTries;
    loopbackHarnessStop(&h);
    lobbyBotPoolsReset();

    UT_ASSERT_MSG(at >= 0, "the client never settled its lobby sync");
    UT_ASSERT_MSG(st == CLIENT_BOT_POOL_S_HAVE,
                  "a client holding the server's pools is in state %u, not "
                  "HAVE", (unsigned)st);
    UT_ASSERT_MSG(tries == 0,
                  "a client holding the server's pools asked for them %u "
                  "time(s)", (unsigned)tries);
    return 0;
}

/* ── Taking the catalogue again after the sim exists ────────────────── */

int run_bot_pool_refresh_retakes_catalogue(void) {
    BYTE           emap[6000] = E_MAP;
    ServerSim     *sim;
    ControlEvent   evt;
    const uint8_t *blob = NULL;
    uint32_t       blobLen = 0, blobId = 0;
    uint32_t       firstId, newId;

    if (!threadsCreate(TRUE)) UT_FAIL("threadsCreate failed");
    lobbyBotPoolsReset();
    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island", gameOpen,
                                    false, 0, -1);
    UT_ASSERT(sim != NULL);
    firstId = sim->botPoolId;
    UT_ASSERT_MSG(firstId != 0, "the sim took no catalogue of the built-in "
                  "pools");

    /* Pools loaded after the sim was made, as WinBoloDS loads them. */
    UT_ASSERT(installOtherPools());
    newId = lobbyBotPoolsCatalogId();
    UT_ASSERT_MSG(sim->botPoolId == firstId,
                  "loading pools changed the sim's catalogue without a "
                  "refresh");
    UT_ASSERT_MSG(sim->botPoolId != newId,
                  "the new pools have the built-in pools' id");

    serverSimRefreshBotPools(sim);
    UT_ASSERT_MSG(sim->botPoolId == newId,
                  "after the refresh the sim holds id %08x, the loaded pools' "
                  "is %08x", (unsigned)sim->botPoolId, (unsigned)newId);

    serverSimFillBotPoolInfoEvent(sim, &evt);
    UT_ASSERT(evt.type == CTRL_LOBBY_BOT_POOL_INFO);
    UT_ASSERT_MSG(evt.u.lobbyBotPoolInfo.id == newId &&
                  evt.u.lobbyBotPoolInfo.len == sim->botPoolBlobLen,
                  "the info event names id %08x length %u, the sim holds "
                  "%08x length %u", (unsigned)evt.u.lobbyBotPoolInfo.id,
                  (unsigned)evt.u.lobbyBotPoolInfo.len, (unsigned)newId,
                  (unsigned)sim->botPoolBlobLen);

    UT_ASSERT(serverSimGetBotPoolBlob(sim, &blob, &blobLen, &blobId));
    UT_ASSERT(blob != NULL && blobLen > 0 && blobId == newId);

    /* The blob handed out is the new pools: installed over the built-in
     * ones, it comes out with the same id. */
    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsCatalogId() != newId);
    UT_ASSERT_MSG(lobbyBotPoolsDeserializeInstall(blob, (int)blobLen, NULL) > 0,
                  "the refreshed blob did not install");
    UT_ASSERT_MSG(lobbyBotPoolsCatalogId() == newId,
                  "the refreshed blob installs as id %08x, want %08x",
                  (unsigned)lobbyBotPoolsCatalogId(), (unsigned)newId);

    serverSimDestroy(sim);
    threadsDestroy();
    lobbyBotPoolsReset();
    return 0;
}

/* ── A bulk re-base while the catalogue is arriving ─────────────────── */

#define NOISY_POOLS  8
#define NOISY_NAMES  200
#define NOISY_LEN    20

static char        s_noisyNames[NOISY_POOLS][NOISY_NAMES][NOISY_LEN + 1];
static const char *s_noisyPtrs[NOISY_POOLS][NOISY_NAMES];

/* Pools of random names, which compress far less than the built-in ones, so
 * the server's catalogue spans many bulk segments and takes the server more
 * than one tick to send. */
static bool installNoisyPools(uint32_t seed) {
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static char       labels[NOISY_POOLS][16];
    LobbyBotPoolDef   defs[NOISY_POOLS];
    int               p, n, k;

    for (p = 0; p < NOISY_POOLS; p++) {
        for (n = 0; n < NOISY_NAMES; n++) {
            for (k = 0; k < NOISY_LEN; k++) {
                seed = seed * 1103515245u + 12345u;
                s_noisyNames[p][n][k] =
                    chars[(seed >> 16) % (sizeof(chars) - 1)];
            }
            s_noisyNames[p][n][NOISY_LEN] = '\0';
            s_noisyPtrs[p][n] = s_noisyNames[p][n];
        }
        SDL_snprintf(labels[p], sizeof(labels[p]), "Noisy %d", p);
        defs[p].label     = labels[p];
        defs[p].names     = s_noisyPtrs[p];
        defs[p].nameCount = NOISY_NAMES;
    }
    return lobbyBotPoolsInstall(defs, NOISY_POOLS, NULL) == NOISY_POOLS;
}

int run_loopback_bot_pool_survives_bulk_rebase(void) {
    LoopbackHarness h;
    ControlEvent    evt;
    uint32_t        serverId;
    BYTE            slot;
    int             at, i;
    uint8_t         st;
    bool            caught = false;

    /* The server takes its copy of these when the harness makes the sim. */
    lobbyBotPoolsReset();
    UT_ASSERT(installNoisyPools(7u));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "PoolRebase", /*lobbyMode*/ true,
                                       NULL, /*seed*/ 0xB0A9u),
                  "harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    serverId = h.sim->botPoolId;
    UT_ASSERT(serverId != 0 && serverId == lobbyBotPoolsCatalogId());
    fprintf(stderr, "  bot pool catalogue: %u bytes on the wire\n",
            (unsigned)h.sim->botPoolBlobLen);
    /* The server's clients[] slot for this player is its player number. */
    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("the client holds no player slot");
    }

    /* The client's table changes under it, as in loopback_bot_pool_fetch. */
    UT_ASSERT(installOtherPools());
    UT_ASSERT(lobbyBotPoolsCatalogId() != serverId);
    h.cs->lobbyPoolState = CLIENT_BOT_POOL_S_NONE;
    threadsWaitForMutex();
    serverSimFillBotPoolInfoEvent(h.sim, &evt);
    publishCb(h.sim, &evt);
    threadsReleaseMutex();

    /* Step the client and the server one at a time, the order
     * loopbackHarnessPump uses, and look between the two: the first time the
     * client has read part of the answer, the server has not yet ticked
     * again, so the rest is not on the wire yet and the re-base below cuts it
     * off. */
    for (i = 0; i < FETCH_MAX; i++) {
        loopbackHarnessPumpClientOnly(&h, 1);
        if (transportUdpClientTestBotPoolArriving(&h.cs->transport)) {
            caught = true;
            break;
        }
        if (h.cs->lobbyPoolState == CLIENT_BOT_POOL_S_HAVE ||
            h.cs->lobbyPoolState == CLIENT_BOT_POOL_S_FAILED) {
            break;
        }
        SDL_Delay(1);   /* let the server's recv thread queue the request */
        serverInstanceTick(h.sim);
    }
    if (!caught) {
        st = h.cs->lobbyPoolState;
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("the catalogue answer was never seen part way in (state %u "
                "after %d pumps)", (unsigned)st, i);
    }
    UT_ASSERT(h.cs->lobbyPoolState == CLIENT_BOT_POOL_S_ASKED);

    /* The server re-bases this player's bulk channel, as a lobby map change
     * does: the rest of the catalogue answer is never sent. */
    threadsWaitForMutex();
    serverRebaseBulkAndRearmDownload((int)slot);
    threadsReleaseMutex();

    /* The client drops the cut-off answer when the channel reset reaches it
     * and sends the request again in the same tick. */
    at = loopbackHarnessPumpUntil(&h, FETCH_MAX, pred_pool_settled, NULL);
    st = h.cs->lobbyPoolState;
    fprintf(stderr, "  bot pool after bulk rebase: caught@%d settled@%d "
            "state=%u tries=%u\n", i, at, (unsigned)st,
            (unsigned)h.cs->lobbyPoolTries);
    if (at < 0 || st != CLIENT_BOT_POOL_S_HAVE) {
        loopbackHarnessStop(&h);
        lobbyBotPoolsReset();
        UT_FAIL("the catalogue did not arrive within %d pumps of the bulk "
                "rebase (state %u)", FETCH_MAX, (unsigned)st);
    }
    UT_ASSERT_MSG(lobbyBotPoolsCatalogId() == serverId,
                  "the client installed a catalogue whose id is %08x, the "
                  "server's is %08x", (unsigned)lobbyBotPoolsCatalogId(),
                  (unsigned)serverId);
    /* Without the re-base handling the client still arrives here, but only
     * after the request times out and is sent a second time. With it, the
     * re-base clears the tries and the request goes out again at once. */
    UT_ASSERT_MSG(h.cs->lobbyPoolTries == 1 &&
                  at < CLIENT_BOT_POOL_TIMEOUT_TICKS,
                  "the catalogue was asked for again only after a timeout "
                  "(tries %u, settled %d pumps after the re-base)",
                  (unsigned)h.cs->lobbyPoolTries, at);

    loopbackHarnessStop(&h);
    lobbyBotPoolsReset();
    return 0;
}

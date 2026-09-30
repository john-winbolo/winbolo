/*
 * Coverage for CTRL_ALLIANCE_RESET — the batched alliance event that
 * replaces the O(N²) per-pair CTRL_ALLIANCE_ACCEPT burst that
 * serverSimReapplyTeamAlliances used to fan out at game start. The
 * burst was filling slot 0's 128-deep reliable control queue faster
 * than the host's main thread could send ACKs back over UDP loopback,
 * which kicked the host from their own server and crashed via the
 * recursive "X has left." broadcast path.
 *
 *   1. Codec round-trip preserves the per-player ally bitmap.
 *   2. Bounds checks — short bodies are rejected by the decoder.
 *   3. serverSimReapplyTeamAlliances publishes exactly ONE
 *      CTRL_ALLIANCE_RESET event with a correct matrix, regardless of
 *      player count. Previously it fanned out N×(N-1)/2 events.
 *   4. Client apply rebuilds the alliance state from the matrix —
 *      pre-existing alliances are cleared, then re-accepted per the
 *      bitmap (mirrors the server's reapply).
 *   5/6. After a game start on either start path, every in-process
 *      bot's ClientSim alliance matrix matches the server's, so a
 *      bot's view never paints its own team as enemies.
 *   7/8. Applying a reset moves no pillbox and no base owner, whether
 *      the matrix it carries is the one already in force or a
 *      different one. Ownership changes hands on a departure, which
 *      arrives as CTRL_PLAYER_LEAVE.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bot_manager.h"             /* botManagerOnGameStart, BotContext */
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "transport_udp_internal.h"  /* PACKET_HEADER_SIZE, MAX_CONTROL_PACKET */
#include "netpacks.h"                /* PACKET_ALLIANCE_UPDATE, ALLIANCE_EVENT_RESET */
#include "server_sim.h"
#include "server_sim_internal.h"     /* sim->lobbyPlayers, playerConnected */
#include "server_sim_lifecycle.h"
#include "players.h"                 /* playersIsAllie */
#include "pillbox.h"                 /* the owners a reset must not move */
#include "bases.h"
#include "tank.h"                    /* tankGetWorld, for the pill check */
#include "server_sim_scenario.h"     /* serverSimApplyScenarioOp */
#include "everard_map.h"
#include "test_harness.h"

/* ----------------------------------------------------------------
 * Codec helpers
 * ---------------------------------------------------------------- */

static int codec_roundtrip_reset(const ControlEvent *in, ControlEvent *out) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_ALLIANCE_RESET);
    if (enc == NULL) return -1;
    EncodeResult r = enc(in, NULL, buf, sizeof(buf), &outLen);
    if (r != ENCODE_OK) return -2;
    if (outLen < PACKET_HEADER_SIZE) return -3;
    /* packHeader writes packet type at byte 2. */
    uint8_t packetType = buf[2];
    if (packetType != PACKET_ALLIANCE_UPDATE) return -4;
    /* The discriminator byte immediately follows the header — must be
     * the RESET sub-type for the wire dispatcher to forward to the
     * right body decoder. */
    if (buf[PACKET_HEADER_SIZE] != ALLIANCE_EVENT_RESET) return -5;
    ControlDecodeFn dec = transportControlCodecDecoder(packetType);
    if (dec == NULL) return -6;
    if (!dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, out)) {
        return -7;
    }
    return 0;
}

/* ================================================================
 * 1. Codec round-trip — the per-player ally bitmap survives.
 *
 * Mix of patterns: solo (self-bit only), pair, full team of 4, an
 * unconnected slot left zero. Catches off-by-one in the wire layout
 * and confirms PACKET_ALLIANCE_UPDATE + ALLIANCE_EVENT_RESET routing.
 * ================================================================ */
int run_alliance_reset_codec_roundtrip(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.type = CTRL_ALLIANCE_RESET;

    /* Slot 0 solo; slots 1+2 paired; slots 3+4+5 triple; slot 6
     * connected but unaffiliated (self-bit only); slots 7..15 unused
     * (zero). */
    in.u.allianceReset.allies[0] = (uint16_t)(1u << 0);
    in.u.allianceReset.allies[1] = (uint16_t)((1u << 1) | (1u << 2));
    in.u.allianceReset.allies[2] = (uint16_t)((1u << 1) | (1u << 2));
    in.u.allianceReset.allies[3] = (uint16_t)((1u << 3) | (1u << 4) | (1u << 5));
    in.u.allianceReset.allies[4] = (uint16_t)((1u << 3) | (1u << 4) | (1u << 5));
    in.u.allianceReset.allies[5] = (uint16_t)((1u << 3) | (1u << 4) | (1u << 5));
    in.u.allianceReset.allies[6] = (uint16_t)(1u << 6);
    /* 7..15 left zero. */

    UT_ASSERT_MSG(codec_roundtrip_reset(&in, &out) == 0,
                  "codec round-trip failed");
    UT_ASSERT(out.type == CTRL_ALLIANCE_RESET);
    for (int i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(out.u.allianceReset.allies[i]
                          == in.u.allianceReset.allies[i],
                      "allies[%d] mismatch: got 0x%04X want 0x%04X",
                      i,
                      (unsigned)out.u.allianceReset.allies[i],
                      (unsigned)in.u.allianceReset.allies[i]);
    }
    return 0;
}

/* ================================================================
 * 2. Decoder bounds — refuses bodies shorter than the matrix length.
 *
 * The reliable carrier feeds the body decoder directly (no
 * discriminator byte); the body needs 2*MAX_TANKS bytes. Anything
 * shorter must return false instead of reading off the end.
 * ================================================================ */
int run_alliance_reset_decoder_rejects_short(void) {
    ControlEvent out;
    uint8_t shortBuf[2 * MAX_TANKS - 1];
    memset(shortBuf, 0, sizeof(shortBuf));

    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ALLIANCE_RESET);
    UT_ASSERT(dec != NULL);

    UT_ASSERT_MSG(!dec(shortBuf, sizeof(shortBuf), &out),
                  "decoder accepted a body one byte short of the matrix");

    /* Exact length must succeed (sanity-check the boundary). */
    uint8_t okBuf[2 * MAX_TANKS];
    memset(okBuf, 0xAB, sizeof(okBuf));
    UT_ASSERT_MSG(dec(okBuf, sizeof(okBuf), &out),
                  "decoder rejected an exact-length body");
    UT_ASSERT(out.type == CTRL_ALLIANCE_RESET);
    UT_ASSERT_MSG(out.u.allianceReset.allies[0] == 0xABABu,
                  "body bytes failed to populate allies[0] "
                  "(got 0x%04X want 0xABAB)",
                  (unsigned)out.u.allianceReset.allies[0]);
    return 0;
}

/* ----------------------------------------------------------------
 * Server-side counting subscriber.
 * ---------------------------------------------------------------- */

typedef struct {
    int resetCount;
    int acceptCount;
    int leaveCount;
    int totalEvents;
    ControlEvent lastReset;
} AllianceCounter;

static void count_alliance_events(void *ctx, const ControlEvent *evt) {
    AllianceCounter *c = (AllianceCounter *)ctx;
    c->totalEvents++;
    if (evt->type == CTRL_ALLIANCE_RESET) {
        c->resetCount++;
        c->lastReset = *evt;
    } else if (evt->type == CTRL_ALLIANCE_ACCEPT) {
        c->acceptCount++;
    } else if (evt->type == CTRL_ALLIANCE_LEAVE) {
        c->leaveCount++;
    }
}

static ServerSim *make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* ================================================================
 * 3. serverSimReapplyTeamAlliances publishes exactly one
 *    CTRL_ALLIANCE_RESET — not the historical N×(N-1)/2 burst.
 *
 * Set up six players: slots 0/1/2 on team 1, slots 3/4 on team 2,
 * slot 5 unaffiliated (team 0). Pre-fix this would have fanned out
 * 3 + 1 = 4 CTRL_ALLIANCE_ACCEPT events (team-1 has C(3,2)=3 pairs;
 * team-2 has C(2,2)=1 pair). Post-fix: exactly one CTRL_ALLIANCE_RESET
 * carrying the full 6-slot matrix. The matrix must reflect the team
 * groupings and set the self-bit for every connected slot (including
 * the unaffiliated one, so the receiver knows the slot exists).
 * ================================================================ */
int run_alliance_reset_reapply_publishes_one_event(void) {
    ServerSim *sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    /* Seed the lobby: 6 connected slots, two teams + one solo. */
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    serverSimAddPlayer(sim, 3, "P3", false);
    serverSimAddPlayer(sim, 4, "P4", false);
    serverSimAddPlayer(sim, 5, "P5", false);
    /* serverSimSetTeamBatch skips the auto-rebake — we want to drive
     * the rebake ourselves so the counter captures only that pass. */
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 1);
    serverSimSetTeamBatch(sim, 3, 2);
    serverSimSetTeamBatch(sim, 4, 2);
    serverSimSetTeamBatch(sim, 5, 0);

    /* Subscribe AFTER the join/team setup so the sync replay's
     * baseline doesn't pollute the counter — the counter must measure
     * just the reapplyTeamAlliances publish. */
    AllianceCounter counter;
    memset(&counter, 0, sizeof(counter));
    SubscriberHandle h =
        serverSimRegisterSubscriber(sim, count_alliance_events, &counter);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Snapshot the alliance counts after sync replay so we measure
     * deltas. The sync replay itself does NOT emit alliance events
     * today (alliances ride in CTRL_PLAYER_JOIN's allies[]) — pin
     * that as part of this test. */
    UT_ASSERT_MSG(counter.resetCount == 0,
                  "sync replay must not emit CTRL_ALLIANCE_RESET "
                  "(got %d)", counter.resetCount);
    UT_ASSERT_MSG(counter.acceptCount == 0,
                  "sync replay must not emit CTRL_ALLIANCE_ACCEPT "
                  "(got %d)", counter.acceptCount);

    serverSimReapplyTeamAlliances(sim);

    UT_ASSERT_MSG(counter.resetCount == 1,
                  "reapplyTeamAlliances must publish exactly ONE "
                  "CTRL_ALLIANCE_RESET (got %d)", counter.resetCount);
    UT_ASSERT_MSG(counter.acceptCount == 0,
                  "reapplyTeamAlliances must NOT publish per-pair "
                  "CTRL_ALLIANCE_ACCEPT (got %d) — that was the "
                  "queue-overflow source the batch is replacing",
                  counter.acceptCount);

    /* Matrix shape: team-1 (slots 0/1/2) and team-2 (slots 3/4) must
     * be fully connected. Slot 5 is unaffiliated — self-bit only. */
    const uint16_t *am = counter.lastReset.u.allianceReset.allies;
    const uint16_t team1 = (uint16_t)((1u << 0) | (1u << 1) | (1u << 2));
    const uint16_t team2 = (uint16_t)((1u << 3) | (1u << 4));
    UT_ASSERT_MSG(am[0] == team1, "slot 0 allies=0x%04X want 0x%04X",
                  (unsigned)am[0], (unsigned)team1);
    UT_ASSERT_MSG(am[1] == team1, "slot 1 allies=0x%04X want 0x%04X",
                  (unsigned)am[1], (unsigned)team1);
    UT_ASSERT_MSG(am[2] == team1, "slot 2 allies=0x%04X want 0x%04X",
                  (unsigned)am[2], (unsigned)team1);
    UT_ASSERT_MSG(am[3] == team2, "slot 3 allies=0x%04X want 0x%04X",
                  (unsigned)am[3], (unsigned)team2);
    UT_ASSERT_MSG(am[4] == team2, "slot 4 allies=0x%04X want 0x%04X",
                  (unsigned)am[4], (unsigned)team2);
    UT_ASSERT_MSG(am[5] == (uint16_t)(1u << 5),
                  "slot 5 (unaffiliated but connected) should have "
                  "only self-bit set, got 0x%04X", (unsigned)am[5]);
    /* Unused slots stay zero. */
    for (int i = 6; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(am[i] == 0,
                      "slot %d (unused) should be 0, got 0x%04X",
                      i, (unsigned)am[i]);
    }

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. Client apply: CTRL_ALLIANCE_RESET rebuilds the alliance state.
 *
 * Seed the client with pre-existing alliances that DO NOT match the
 * incoming reset — that proves the apply clears before re-accepting.
 * Then send a RESET that pairs slots 0/1 and triples slots 2/3/4, and
 * verify playersIsAllie agrees with the new matrix while the stale
 * pair is gone.
 * ================================================================ */
int run_alliance_reset_apply_rebuilds_alliances(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    /* Seed the ClientSim's players struct: add slots 0..4 inUse so
     * playersAcceptAlliance / playersLeaveAlliance have valid targets.
     * The test bypasses the network layer — we operate directly on
     * the underlying GameSim's players struct via the client apply
     * path, so the inUse flags need to be set up by hand. */
    GameSim *gs = clientSimGetGameSim(cs);
    for (BYTE i = 0; i < 5; i++) {
        gs->plyrs->item[i].inUse = TRUE;
    }

    /* Pre-existing stale alliance: slot 0 + slot 4. The reset below
     * does NOT include this pair, so it must be cleared by the apply. */
    playersAcceptAlliance(gs, &gs->plyrs, /*selfPlayer*/ 0, /*a*/ 0, /*b*/ 4, FALSE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 4),
                  "test setup: stale 0+4 alliance must be in place "
                  "before the reset");

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_RESET;
    /* New matrix: 0+1 paired, 2+3+4 tripled. */
    evt.u.allianceReset.allies[0] = (uint16_t)((1u << 0) | (1u << 1));
    evt.u.allianceReset.allies[1] = (uint16_t)((1u << 0) | (1u << 1));
    evt.u.allianceReset.allies[2] = (uint16_t)((1u << 2) | (1u << 3) | (1u << 4));
    evt.u.allianceReset.allies[3] = (uint16_t)((1u << 2) | (1u << 3) | (1u << 4));
    evt.u.allianceReset.allies[4] = (uint16_t)((1u << 2) | (1u << 3) | (1u << 4));

    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1),
                  "after reset: 0+1 must be allied");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 2, 3),
                  "after reset: 2+3 must be allied");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 3, 4),
                  "after reset: 3+4 must be allied");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 2, 4),
                  "after reset: 2+4 must be allied (transitive)");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 0, 4),
                  "after reset: stale 0+4 alliance must be cleared");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 1, 2),
                  "after reset: 1+2 (cross-group) must NOT be allied");

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 5. In-place start: each bot's ClientSim alliance matrix ends up
 *    identical to the server's.
 *
 * serverSimStartGameInPlace re-arms the bots first
 * (botManagerOnGameStart reloads each bot's map and recreates its
 * tank) and only then calls serverSimReapplyTeamAlliances, whose
 * botManagerSyncClientAlliances copies the server matrix into every
 * in-process bot ClientSim. The renderer colours tanks from the
 * followed bot's CLIENT players object, so a bot left without those
 * bits draws its own teammates as enemies.
 *
 * Slot 0 is a human on team 1 with bots 1 and 2, so a human/bot pair
 * and a bot/bot pair are both covered; bots 3 and 4 are team 2.
 *
 * The bot ClientSims are patched straight into botMgr.bots[] rather
 * than going through botManagerAddBot, which would spin a real Lua VM
 * and need a brain script on disk.
 * ================================================================ */
int run_alliance_reset_bots_synced_after_inplace_start(void) {
    ClientSim *botCs[MAX_TANKS];
    ServerSim *sim;
    GameSim   *srvGs;
    BYTE b, i;

    memset(botCs, 0, sizeof(botCs));
    sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Human", false);
    serverSimAddPlayer(sim, 1, "Bot1", false);
    serverSimAddPlayer(sim, 2, "Bot2", false);
    serverSimAddPlayer(sim, 3, "Bot3", false);
    serverSimAddPlayer(sim, 4, "Bot4", false);
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 1);
    serverSimSetTeamBatch(sim, 3, 2);
    serverSimSetTeamBatch(sim, 4, 2);

    srvGs = serverSimGetGameSim(sim);
    for (b = 1; b <= 4; b++) {
        srvGs->plyrs->item[b].clientFlags |= PLAYER_FLAG_BOT;
    }

    /* One real ClientSim per bot slot, wired into botMgr.bots[] the way
     * botManagerAddBot leaves them: .active set and .cs pointing at the
     * bot's own sim. The roster slots are marked inUse by hand — over
     * the wire that comes from CTRL_PLAYER_JOIN, which these unhooked
     * ClientSims never receive, and playersIsAllie needs it. */
    for (b = 1; b <= 4; b++) {
        GameSim *botGs;
        botCs[b] = clientSimAlloc();
        UT_ASSERT(botCs[b] != NULL);
        clientSimCreate(botCs[b]);
        clientSimSetPlayerNum(botCs[b], b);
        botGs = clientSimGetGameSim(botCs[b]);
        for (i = 0; i < 5; i++) {
            botGs->plyrs->item[i].inUse = TRUE;
        }
        sim->botMgr.bots[b].active = true;
        sim->botMgr.bots[b].cs     = botCs[b];
    }

    /* The in-place path calls botManagerOnGameStart itself. */
    serverSimStartGameInPlace(sim);

    srvGs = serverSimGetGameSim(sim);
    for (b = 1; b <= 4; b++) {
        players *botPlrs = &clientSimGetGameSim(botCs[b])->plyrs;
        for (i = 0; i < MAX_TANKS; i++) {
            UT_ASSERT_MSG((*botPlrs)->item[i].allie
                              == srvGs->plyrs->item[i].allie,
                          "in-place start: bot %u disagrees with the server "
                          "on slot %u (bot 0x%04X, server 0x%04X)",
                          (unsigned)b, (unsigned)i,
                          (unsigned)(*botPlrs)->item[i].allie,
                          (unsigned)srvGs->plyrs->item[i].allie);
        }

        /* The rendering question, asked directly: does this bot see its
         * own team as allies and the other team as enemies? */
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 0, 1),
                      "in-place start: bot %u must see 0+1 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 0, 2),
                      "in-place start: bot %u must see 0+2 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 1, 2),
                      "in-place start: bot %u must see 1+2 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 3, 4),
                      "in-place start: bot %u must see 3+4 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(!playersIsAllie(botPlrs, 1, 3),
                      "in-place start: bot %u must NOT see 1+3 allied "
                      "(team 1 vs team 2)", (unsigned)b);
        UT_ASSERT_MSG(!playersIsAllie(botPlrs, 0, 4),
                      "in-place start: bot %u must NOT see 0+4 allied "
                      "(team 1 vs team 2)", (unsigned)b);
    }

    /* Detach before teardown so botManagerDestroy (from serverSimDestroy)
     * never tears down a brain that was never created. */
    for (b = 1; b <= 4; b++) {
        sim->botMgr.bots[b].active = false;
        sim->botMgr.bots[b].cs     = NULL;
    }
    for (b = 1; b <= 4; b++) {
        if (botCs[b] != NULL) clientSimDestroy(botCs[b]);
    }
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. Countdown start: same bitmap agreement over the other order.
 *
 * serverSimStartGame runs serverSimReapplyTeamAlliances (and with it
 * botManagerSyncClientAlliances) inside itself; production re-arms the
 * bots afterwards, from the countdown->running transition in
 * server_lifecycle.c. So the sync lands first here and the map reload
 * plus tank recreate follow it — the reverse of the in-place path.
 * Same seating and the same assertions, so a difference between the
 * two cases is a difference in call order and nothing else.
 * ================================================================ */
int run_alliance_reset_bots_synced_after_countdown_start(void) {
    ClientSim *botCs[MAX_TANKS];
    ServerSim *sim;
    GameSim   *srvGs;
    BYTE b, i;

    memset(botCs, 0, sizeof(botCs));
    sim = make_lobby_sim();
    UT_ASSERT(sim != NULL);

    serverSimAddPlayer(sim, 0, "Human", false);
    serverSimAddPlayer(sim, 1, "Bot1", false);
    serverSimAddPlayer(sim, 2, "Bot2", false);
    serverSimAddPlayer(sim, 3, "Bot3", false);
    serverSimAddPlayer(sim, 4, "Bot4", false);
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 1);
    serverSimSetTeamBatch(sim, 3, 2);
    serverSimSetTeamBatch(sim, 4, 2);

    srvGs = serverSimGetGameSim(sim);
    for (b = 1; b <= 4; b++) {
        srvGs->plyrs->item[b].clientFlags |= PLAYER_FLAG_BOT;
    }

    for (b = 1; b <= 4; b++) {
        GameSim *botGs;
        botCs[b] = clientSimAlloc();
        UT_ASSERT(botCs[b] != NULL);
        clientSimCreate(botCs[b]);
        clientSimSetPlayerNum(botCs[b], b);
        botGs = clientSimGetGameSim(botCs[b]);
        for (i = 0; i < 5; i++) {
            botGs->plyrs->item[i].inUse = TRUE;
        }
        sim->botMgr.bots[b].active = true;
        sim->botMgr.bots[b].cs     = botCs[b];
    }

    /* Production's countdown->running order (server_lifecycle.c): the
     * full-reset start first, the bot re-arm second. */
    serverSimStartGame(sim);
    botManagerOnGameStart(sim);

    srvGs = serverSimGetGameSim(sim);
    for (b = 1; b <= 4; b++) {
        players *botPlrs = &clientSimGetGameSim(botCs[b])->plyrs;
        for (i = 0; i < MAX_TANKS; i++) {
            UT_ASSERT_MSG((*botPlrs)->item[i].allie
                              == srvGs->plyrs->item[i].allie,
                          "countdown start: bot %u disagrees with the server "
                          "on slot %u (bot 0x%04X, server 0x%04X)",
                          (unsigned)b, (unsigned)i,
                          (unsigned)(*botPlrs)->item[i].allie,
                          (unsigned)srvGs->plyrs->item[i].allie);
        }

        UT_ASSERT_MSG(playersIsAllie(botPlrs, 0, 1),
                      "countdown start: bot %u must see 0+1 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 0, 2),
                      "countdown start: bot %u must see 0+2 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 1, 2),
                      "countdown start: bot %u must see 1+2 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(playersIsAllie(botPlrs, 3, 4),
                      "countdown start: bot %u must see 3+4 allied",
                      (unsigned)b);
        UT_ASSERT_MSG(!playersIsAllie(botPlrs, 1, 3),
                      "countdown start: bot %u must NOT see 1+3 allied "
                      "(team 1 vs team 2)", (unsigned)b);
        UT_ASSERT_MSG(!playersIsAllie(botPlrs, 0, 4),
                      "countdown start: bot %u must NOT see 0+4 allied "
                      "(team 1 vs team 2)", (unsigned)b);
    }

    for (b = 1; b <= 4; b++) {
        sim->botMgr.bots[b].active = false;
        sim->botMgr.bots[b].cs     = NULL;
    }
    for (b = 1; b <= 4; b++) {
        if (botCs[b] != NULL) clientSimDestroy(botCs[b]);
    }
    serverSimDestroy(sim);
    return 0;
}

/* ----------------------------------------------------------------
 * Ownership across a reset.
 *
 * A pillbox or base changes hands when its owner leaves, and the
 * receiver is told that by CTRL_PLAYER_LEAVE. CTRL_ALLIANCE_RESET says
 * who is allied with whom and nothing about anybody leaving, so applying
 * one must leave every owner where it is. The clearing pass used to run
 * playersLeaveAlliance over all sixteen slots, which handed each slot's
 * pillboxes and bases to the first ally it could find — or to slot 15
 * when it could find none — and the re-accept pass put the bits back and
 * not the ownership. The viewer's own pillboxes then drew hostile until
 * the next full pill snapshot, up to 250 ticks later.
 * ---------------------------------------------------------------- */

/* Four pillboxes and three bases, owned by the viewer, an ally, a
 * stranger and nobody, so a migration to an ally and a migration to the
 * top slot are both visible. */
#define AR_PILLS 4
#define AR_BASES 3

static void ar_seed_world(GameSim *gs) {
    /* Both setters mark every item within the new count live, which is
       the state the migration walks, and pillsCreate leaves each pillbox
       out of a tank, which is the other half of what it reads. */
    pillsSetNumPills(&gs->pb, AR_PILLS);
    (*gs->pb).item[0].owner = 0;        /* the viewer's own    */
    (*gs->pb).item[1].owner = 1;        /* an ally's           */
    (*gs->pb).item[2].owner = 2;        /* a stranger's        */
    (*gs->pb).item[3].owner = NEUTRAL;  /* nobody's            */

    basesSetNumBases(&gs->bs, AR_BASES);
    (*gs->bs).item[0].owner = 0;
    (*gs->bs).item[1].owner = 3;
    (*gs->bs).item[2].owner = NEUTRAL;
}

static int ar_owners_unchanged(GameSim *gs, const BYTE *wantPills,
                               const BYTE *wantBases, const char *when) {
    BYTE i;

    for (i = 0; i < AR_PILLS; i++) {
        UT_ASSERT_MSG(pillsGetPillOwner(&gs->pb, (BYTE)(i + 1)) == wantPills[i],
                      "%s: pillbox %u is owned by %u, expected %u",
                      when, (unsigned)(i + 1),
                      (unsigned)pillsGetPillOwner(&gs->pb, (BYTE)(i + 1)),
                      (unsigned)wantPills[i]);
    }
    for (i = 0; i < AR_BASES; i++) {
        UT_ASSERT_MSG(basesGetBaseOwner(&gs->bs, (BYTE)(i + 1)) == wantBases[i],
                      "%s: base %u is owned by %u, expected %u",
                      when, (unsigned)(i + 1),
                      (unsigned)basesGetBaseOwner(&gs->bs, (BYTE)(i + 1)),
                      (unsigned)wantBases[i]);
    }
    return 0;
}

/* A reset that says exactly what is already in force. */
int run_alliance_reset_apply_keeps_owners(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    ControlEvent evt;
    const BYTE wantPills[AR_PILLS] = { 0, 1, 2, NEUTRAL };
    const BYTE wantBases[AR_BASES] = { 0, 3, NEUTRAL };
    const uint16_t pair01 = (uint16_t)((1u << 0) | (1u << 1));
    const uint16_t pair23 = (uint16_t)((1u << 2) | (1u << 3));

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    gs = clientSimGetGameSim(cs);
    for (BYTE i = 0; i < 5; i++) {
        gs->plyrs->item[i].inUse = TRUE;
    }

    playersAcceptAlliance(gs, &gs->plyrs, 0, 0, 1, FALSE);
    playersAcceptAlliance(gs, &gs->plyrs, 0, 2, 3, FALSE);
    UT_ASSERT(playersIsAllie(&gs->plyrs, 0, 1));
    UT_ASSERT(playersIsAllie(&gs->plyrs, 2, 3));

    ar_seed_world(gs);
    if (ar_owners_unchanged(gs, wantPills, wantBases, "test setup") != 0) {
        clientSimDestroy(cs);
        return 1;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_RESET;
    evt.u.allianceReset.allies[0] = pair01;
    evt.u.allianceReset.allies[1] = pair01;
    evt.u.allianceReset.allies[2] = pair23;
    evt.u.allianceReset.allies[3] = pair23;
    evt.u.allianceReset.allies[4] = (uint16_t)(1u << 4);

    clientSimApplyControl(cs, &evt);

    if (ar_owners_unchanged(gs, wantPills, wantBases, "after the reset") != 0) {
        clientSimDestroy(cs);
        return 1;
    }

    /* And the bits the matrix states are the bits in force. */
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1),
                  "after the reset: 0+1 must still be allied");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 2, 3),
                  "after the reset: 2+3 must still be allied");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 0, 2),
                  "after the reset: 0+2 must not be allied");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 1, 4),
                  "after the reset: 1+4 must not be allied");

    clientSimDestroy(cs);
    return 0;
}

/* And a reset that genuinely changes the matrix: the bits follow it, the
 * owners still do not move. Only a departure moves those. */
int run_alliance_reset_changed_matrix_keeps_owners(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    ControlEvent evt;
    const BYTE wantPills[AR_PILLS] = { 0, 1, 2, NEUTRAL };
    const BYTE wantBases[AR_BASES] = { 0, 3, NEUTRAL };
    const uint16_t pair02 = (uint16_t)((1u << 0) | (1u << 2));
    const uint16_t pair13 = (uint16_t)((1u << 1) | (1u << 3));

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    gs = clientSimGetGameSim(cs);
    for (BYTE i = 0; i < 5; i++) {
        gs->plyrs->item[i].inUse = TRUE;
    }

    playersAcceptAlliance(gs, &gs->plyrs, 0, 0, 1, FALSE);
    playersAcceptAlliance(gs, &gs->plyrs, 0, 2, 3, FALSE);
    ar_seed_world(gs);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_RESET;
    evt.u.allianceReset.allies[0] = pair02;
    evt.u.allianceReset.allies[1] = pair13;
    evt.u.allianceReset.allies[2] = pair02;
    evt.u.allianceReset.allies[3] = pair13;
    evt.u.allianceReset.allies[4] = (uint16_t)(1u << 4);

    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 2),
                  "after the reset: 0+2 must be allied");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 1, 3),
                  "after the reset: 1+3 must be allied");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 0, 1),
                  "after the reset: the old 0+1 pair must be gone");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 2, 3),
                  "after the reset: the old 2+3 pair must be gone");

    if (ar_owners_unchanged(gs, wantPills, wantBases,
                            "after a reset that changed the matrix") != 0) {
        clientSimDestroy(cs);
        return 1;
    }

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 9. A set_team mid-game takes the moved player out of his old
 *    team's alliances on the server, not only on the clients.
 *
 * The server used to re-accept on top of what it had, and an accept
 * merges both players' whole ally lists, so the moved player kept his
 * old allies and brought them into the new team. In Virus, which
 * starts everyone on one team and moves them with game.set_team, that
 * left every player allied with every other after the first turn, and
 * no player's pillbox fired at anyone. Driven through the scenario op
 * scnLuaSetTeam builds, and checked on the pillbox as well as the bits.
 * ================================================================ */

/* Put pill 0 beside tank victim, owned by owner and ready to fire, and
 * run one pillsUpdate with only the victim connected. True if the pill
 * picked it as a target. */
static bool ar_pill_targets(ServerSim *sim, BYTE owner, BYTE victim) {
    GameSim *gs = &sim->sim;
    WORLD tx, ty;
    bool conn[MAX_TANKS];

    tankGetWorld(&gs->tanks[victim], &tx, &ty);
    gs->pb->item[0].x        = (BYTE)((tx >> TANK_SHIFT_MAPSIZE) + 1);
    gs->pb->item[0].y        = (BYTE)(ty >> TANK_SHIFT_MAPSIZE);
    gs->pb->item[0].owner    = owner;
    gs->pb->item[0].armour   = 15;
    gs->pb->item[0].inTank   = FALSE;
    gs->pb->item[0].reload   = gs->pb->item[0].speed;
    gs->pb->item[0].justSeen = FALSE;
    gs->pb->active[0]        = TRUE;
    for (BYTE t = 0; t < MAX_TANKS; t++) {
        conn[t] = (t == victim);
    }
    pillsUpdate(gs, gs->tanks, conn, MAX_TANKS);
    return gs->pb->item[0].justSeen == TRUE;
}

int run_alliance_reset_set_team_leaves_old_team(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    ScenarioOp op;
    GameSim *gs;

    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Survivor", false);
    serverSimAddPlayer(sim, 1, "Turned", false);
    serverSimAddPlayer(sim, 2, "Horde", false);
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 2);
    serverSimStartGameInPlace(sim);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(sim->state == serverStateRunning);
    UT_ASSERT(gs->tanks[0] != NULL && gs->tanks[1] != NULL &&
              gs->tanks[2] != NULL);
    UT_ASSERT(playersIsAllie(&gs->plyrs, 0, 1));
    UT_ASSERT(!playersIsAllie(&gs->plyrs, 0, 2));

    /* game.set_team(1, 2), as scnLuaSetTeam hands it to the sim. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SET_TEAM;
    op.u.rosterSetTeam.slot = 1;
    op.u.rosterSetTeam.team = 2;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(sim->lobbyPlayers[1].teamNumber == 2);

    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 0, 1),
                  "after set_team: 0 is still allied with the player "
                  "who left his team");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 1, 2),
                  "after set_team: 1 is not allied with his new team");
    UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 0, 2),
                  "after set_team: the move allied 0 with the other team");

    UT_ASSERT_MSG(ar_pill_targets(sim, 0, 1),
                  "0's pill does not fire at the player who left 0's team");
    UT_ASSERT_MSG(ar_pill_targets(sim, 2, 0),
                  "2's pill does not fire at the other team");
    UT_ASSERT_MSG(!ar_pill_targets(sim, 2, 1),
                  "2's pill fires at 2's new teammate");

    serverSimDestroy(sim);
    return 0;
}

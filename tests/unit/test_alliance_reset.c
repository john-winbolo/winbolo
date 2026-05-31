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
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
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

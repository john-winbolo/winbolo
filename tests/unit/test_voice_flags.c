/*
 * Mic-status visibility in the tank snapshot.
 *
 * PLAYER_FLAG_HAS_MIC / PLAYER_FLAG_VOICE_MUTED are client-reported and
 * ride clientFlags, but serverSimBuildSnapshot only shows them to the
 * players that player's voice could reach: everyone while the server is
 * not running a game (all-talk), the live alliance once it is. This pins
 * that rule, and that it touches nothing but PLAYER_VOICE_FLAG_MASK.
 *
 * Drives ut_make_running_sim (slot 0) plus a second human at slot 1 and
 * reads clientFlags straight off the built TankSnapshot rows. Snapshots
 * are built with noCull so both rows are full rather than hidden stubs —
 * the viewport cull is a separate concern and a stub row carries no
 * clientFlags at all.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"              /* GameSim.plyrs */
#include "players.h"               /* playersSetClientFlags, playersIsAllie */
#include "player_flags.h"
#include "input_packet.h"
#include "test_harness.h"

/* clientFlags of `player`'s row in a snapshot built for `recipient`.
 * Returns 0xFFFF when the row is missing so a caller cannot mistake it
 * for a legitimately cleared byte. */
static unsigned vf_row_flags(ServerSim *sim, BYTE recipient, BYTE player) {
    SnapshotHeader hdr;
    TankSnapshot tk[MAX_TANKS];
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];
    int i;

    serverSimBuildSnapshot(sim, recipient, &hdr, tk, MAX_TANKS,
                           sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS,
                           ev, MAX_SNAPSHOT_EVENTS, true);
    for (i = 0; i < hdr.tankCount; i++) {
        if ((tk[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK) == player &&
            (tk[i].playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0) {
            return tk[i].clientFlags;
        }
    }
    return 0xFFFFu;
}

int run_voice_flags_snapshot_masking(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 are allied by default — breaks the enemy case");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "sim should be running, got %d", (int)serverSimGetState(sim));

    /* Both players claim a mic and are muted. Player 1 also carries a
     * non-voice bit, which no recipient rule may touch. */
    const uint8_t p0Flags = PLAYER_FLAG_HAS_MIC | PLAYER_FLAG_VOICE_MUTED;
    const uint8_t p1Flags = PLAYER_FLAG_HAS_MIC | PLAYER_FLAG_VOICE_MUTED |
                            PLAYER_FLAG_SUPPORTER;
    playersSetClientFlags(&gs->plyrs, 0, p0Flags);
    playersSetClientFlags(&gs->plyrs, 1, p1Flags);

    /* 1. Running, not allied: each recipient loses the other's mic bits and
     *    keeps their own. Player 1's SUPPORTER bit survives the mask. */
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 1) == PLAYER_FLAG_SUPPORTER,
                  "recipient 0: enemy player 1 must lose only the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 1));
    UT_ASSERT_MSG(vf_row_flags(sim, 1, 0) == 0,
                  "recipient 1: enemy player 0's mic bits must be cleared, got 0x%02X",
                  vf_row_flags(sim, 1, 0));
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 0) == p0Flags,
                  "recipient 0: own row must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 0));
    UT_ASSERT_MSG(vf_row_flags(sim, 1, 1) == p1Flags,
                  "recipient 1: own row must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 1, 1));

    /* 2. Running and allied: voice reaches both ways, so both rows keep
     *    the bits. */
    serverSimAcceptAlliance(sim, 0, 1);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "serverSimAcceptAlliance did not ally players 0 and 1");
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 1) == p1Flags,
                  "recipient 0: allied player 1 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 1));
    UT_ASSERT_MSG(vf_row_flags(sim, 1, 0) == p0Flags,
                  "recipient 1: allied player 0 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 1, 0));
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 0) == p0Flags,
                  "recipient 0: own row wrong while allied, got 0x%02X",
                  vf_row_flags(sim, 0, 0));

    /* 3. Not running: the lobby is all-talk, so an un-allied pair still
     *    sees each other's mic state. */
    serverSimLeaveAlliance(sim, 1);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "serverSimLeaveAlliance did not break the alliance");
    serverSimSetState(sim, serverStateLobby);
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 1) == p1Flags,
                  "lobby: un-allied player 1 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 1));
    UT_ASSERT_MSG(vf_row_flags(sim, 1, 0) == p0Flags,
                  "lobby: un-allied player 0 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 1, 0));
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 0) == p0Flags,
                  "lobby: own row wrong, got 0x%02X", vf_row_flags(sim, 0, 0));

    /* The same holds for the other two non-running states. */
    serverSimSetState(sim, serverStateCountdown);
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 1) == p1Flags,
                  "countdown: player 1 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 1));
    serverSimSetState(sim, serverStateGameOver);
    UT_ASSERT_MSG(vf_row_flags(sim, 0, 1) == p1Flags,
                  "game over: player 1 must keep the mic bits, got 0x%02X",
                  vf_row_flags(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

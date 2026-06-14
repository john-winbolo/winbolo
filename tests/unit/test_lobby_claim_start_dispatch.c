/*
 * Coverage for the CMD_LOBBY_CLAIM_START arm in server_command_dispatch.c.
 *
 * The dispatcher gates a start claim behind a lobby-state precondition,
 * target range/connected + startIdx range validation, and an authority
 * split: a player may set only its own start (and only to a free one),
 * while a host may set any slot's start and swaps when the target start
 * is already held. None of that has end-to-end coverage today, so a
 * regression in the swap bookkeeping or the authority check would be
 * silent.
 *
 * Tests build a fresh two-player lobby and drive serverSimApplyCommand
 * directly, holding the threads mutex (the dispatcher asserts it).
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_CLAIM_START, CmdResult */
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.ss for startsGetNumStarts */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled,
                                    * serverSimSetLobbyStartIdx */
#include "starts.h"                /* startsGetNumStarts */
#include "threads.h"
#include "test_harness.h"

/* Host in slot 0 (lobbyClientMayEdit(sim, 0) == true), a non-host human
 * in slot 1. */
static ServerSim *make_two_player_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);
    return sim;
}

static CmdResult apply_claim(ServerSim *sim, int senderSlot,
                             BYTE target, BYTE idx) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_CLAIM_START;
    cmd.cmdSeq = 1;
    cmd.u.lobbyClaimStart.targetSlot = target;
    cmd.u.lobbyClaimStart.startIdx   = idx;
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

static uint8_t start_of(ServerSim *sim, BYTE slot) {
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
    return lp ? lp->startIdx : 0u;
}

/* Host assigning an occupied start swaps the two slots: the assignee
 * takes the requested start and the displaced holder inherits the
 * assignee's old start. */
int run_lobby_claim_start_host_swaps_occupied(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(startsGetNumStarts(&sim->sim.ss) >= 2,
                  "test map needs at least 2 starts");

    serverSimSetLobbyStartIdx(sim, 0, 1);
    serverSimSetLobbyStartIdx(sim, 1, 2);

    /* Host (slot 0) takes Bob's start 2 — swap. */
    UT_ASSERT(apply_claim(sim, 0, 0, 2) == CMD_OK);
    UT_ASSERT_MSG(start_of(sim, 0) == 2,
                  "assignee takes requested start, got %u",
                  (unsigned)start_of(sim, 0));
    UT_ASSERT_MSG(start_of(sim, 1) == 1,
                  "displaced holder inherits assignee's old start, got %u",
                  (unsigned)start_of(sim, 1));

    serverSimDestroy(sim);
    return 0;
}

/* A host assigning an occupied start to a slot whose own start is "none"
 * (0xFF) leaves the displaced holder with 0xFF — the swap is clean even
 * when the assignee held nothing. */
int run_lobby_claim_start_host_swap_into_none(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(startsGetNumStarts(&sim->sim.ss) >= 2);

    serverSimSetLobbyStartIdx(sim, 0, 0xFF);  /* host holds nothing */
    serverSimSetLobbyStartIdx(sim, 1, 2);

    UT_ASSERT(apply_claim(sim, 0, 0, 2) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == 2);
    UT_ASSERT_MSG(start_of(sim, 1) == 0xFF,
                  "displaced holder inherits 'none', got %u",
                  (unsigned)start_of(sim, 1));

    serverSimDestroy(sim);
    return 0;
}

/* A non-host claiming a start another connected slot holds is refused
 * with INVALID and nothing moves. */
int run_lobby_claim_start_non_host_occupied_rejected(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(startsGetNumStarts(&sim->sim.ss) >= 2);

    serverSimSetLobbyStartIdx(sim, 0, 1);
    serverSimSetLobbyStartIdx(sim, 1, 2);

    /* Bob (slot 1) tries to take the host's start 1. */
    UT_ASSERT(apply_claim(sim, 1, 1, 1) == CMD_REJECT_INVALID);
    UT_ASSERT(start_of(sim, 0) == 1);
    UT_ASSERT(start_of(sim, 1) == 2);

    serverSimDestroy(sim);
    return 0;
}

/* A non-host targeting another slot is refused as NOT_HOST regardless of
 * whether the requested start is free. */
int run_lobby_claim_start_non_host_other_slot_rejected(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(startsGetNumStarts(&sim->sim.ss) >= 3);

    serverSimSetLobbyStartIdx(sim, 0, 1);
    serverSimSetLobbyStartIdx(sim, 1, 2);

    UT_ASSERT(apply_claim(sim, 1, 0, 3) == CMD_REJECT_NOT_HOST);
    UT_ASSERT(start_of(sim, 0) == 1);

    serverSimDestroy(sim);
    return 0;
}

/* A non-host self-claim of a free start succeeds, and a 0xFF release of
 * the slot's own start succeeds. */
int run_lobby_claim_start_self_free_and_release(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(startsGetNumStarts(&sim->sim.ss) >= 3);

    serverSimSetLobbyStartIdx(sim, 0, 1);
    serverSimSetLobbyStartIdx(sim, 1, 2);

    /* Bob self-claims a free start 3. */
    UT_ASSERT(apply_claim(sim, 1, 1, 3) == CMD_OK);
    UT_ASSERT(start_of(sim, 1) == 3);

    /* Bob releases. */
    UT_ASSERT(apply_claim(sim, 1, 1, 0xFF) == CMD_OK);
    UT_ASSERT(start_of(sim, 1) == 0xFF);

    serverSimDestroy(sim);
    return 0;
}

/* Range guards: a startIdx past numStarts and a disconnected/out-of-range
 * target are both INVALID. */
int run_lobby_claim_start_validation(void) {
    ServerSim *sim = make_two_player_lobby();
    UT_ASSERT(sim != NULL);

    /* startIdx > numStarts (0xFE is not the 0xFF release sentinel). */
    UT_ASSERT(apply_claim(sim, 0, 0, 0xFE) == CMD_REJECT_INVALID);
    /* Target slot 5 is not connected. */
    UT_ASSERT(apply_claim(sim, 0, 5, 1) == CMD_REJECT_INVALID);

    serverSimDestroy(sim);
    return 0;
}

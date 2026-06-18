/*
 * Regression coverage for the wire player-index bounds hardening
 * (hardening plan §1.2 audit). A hostile or buggy server can put an
 * out-of-range player number in a control event or a player accessor's
 * argument; before the guards these indexed the fixed item[MAX_TANKS]
 * array / lobbySlots[MAX_TANKS] mirror out of bounds (OOB read, and in
 * several cases OOB write). These tests pin the documented safe behaviour
 * so a future change can't quietly drop a guard.
 *
 * Found originally by fuzz_client_snapshot (alliance-leave + truncated
 * event); the rest of the class was found by audit, so this is their
 * deterministic, ASan-independent regression.
 */
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* cs->sim.plyrs */
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"         /* ControlEvent, CTRL_* */
#include "players.h"               /* playersIsAllie, NO_TANK, ... */
#include "test_harness.h"

/* Player accessors must return their documented sentinel for an
 * out-of-range index rather than indexing item[] out of bounds. */
int run_players_oob_index_safe(void) {
    ClientSim *cs = clientSimAlloc();
    players  *plrs;
    char      cc[8];
    char      nm[64];

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    plrs = &cs->sim.plyrs;

    UT_ASSERT(playersIsAllie(plrs, 200, 0)   == FALSE);
    UT_ASSERT(playersIsAllie(plrs, 0,   200) == FALSE);
    UT_ASSERT(playersIsAllie(plrs, 200, 201) == FALSE);

    UT_ASSERT(playersGetAccountFlags(plrs, 200) == 0);

    cc[0] = cc[1] = cc[2] = '!';
    playersGetCountryCode(plrs, 200, cc);
    UT_ASSERT(cc[0] == 'X' && cc[1] == 'X' && cc[2] == '\0');

    playersMakeMessageName(cs, plrs, 0, 200, nm);
    UT_ASSERT(strcmp(nm, NO_TANK) == 0);

    playersGetPlayerName(plrs, 200, nm, FALSE);
    UT_ASSERT(strcmp(nm, NO_TANK) == 0);

    clientSimDestroy(cs);
    return 0;
}

/* clientSimApplyControl must drop control events whose wire player number
 * is out of range (safe no-op), while still applying in-range events. */
int run_control_oob_player_dropped(void) {
    ClientSim   *cs = clientSimAlloc();
    ControlEvent evt;
    const ClientLobbySlot *slot;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_LEAVE;
    evt.u.allianceLeave.playerNum = 200;
    clientSimApplyControl(cs, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 200;
    clientSimApplyControl(cs, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_NAME;
    evt.u.playerName.playerNum = 200;
    clientSimApplyControl(cs, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum = 200;
    clientSimApplyControl(cs, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum = 200;
    clientSimApplyControl(cs, &evt);

    /* In-range slot still applies — the guard must not over-reject. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum = 3;
    evt.u.lobbySlot.slot.connected = TRUE;
    evt.u.lobbySlot.slot.teamNumber = 2;
    clientSimApplyControl(cs, &evt);
    slot = clientSimGetLobbySlot(cs, 3);
    UT_ASSERT(slot != NULL);
    UT_ASSERT(slot->teamNumber == 2);

    clientSimDestroy(cs);
    return 0;
}

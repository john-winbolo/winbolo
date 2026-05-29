/*
 * Coverage for the CTRL_COMMAND_REJECTED arm in
 * client_sim_control.c. The lobby toast (imgui_lobby.cpp) and the
 * in-game drain (sdl3imgui.cpp drainInGameNameReject) both read
 * lobbyLastRejectPacket / lobbyLastRejectReason, so this storage hop
 * is the spine of the name-reject UX path on the client.
 *
 * Tests build a fresh ClientSim with myPlayerNum=0, dispatch a
 * CTRL_COMMAND_REJECTED event through clientSimApplyControl, and
 * assert what landed in lobbyLastRejectPacket / lobbyLastRejectReason
 * via the public getters. CMD_REJECT_NAME_* codes (9..14) are pinned
 * one by one — those are the six the in-game drain maps to lang
 * strings, and the reason byte must round-trip verbatim because the
 * renderer keys off it.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "client_command.h"   /* CMD_NAME_CHANGE, CMD_REJECT_NAME_* */
#include "control_event.h"
#include "test_harness.h"

static ClientSim *fresh_client_sim_as_slot(BYTE me) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, me);
    return cs;
}

static void make_name_reject(ControlEvent *evt, uint8_t origSlot,
                             uint8_t reason) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_COMMAND_REJECTED;
    evt->u.commandRejected.origCmdSeq  = 0;
    evt->u.commandRejected.origCmdType = (uint8_t)CMD_NAME_CHANGE;
    evt->u.commandRejected.reasonCode  = reason;
    evt->u.commandRejected.origSlot    = origSlot;
}

/* Each CMD_REJECT_NAME_* code parks origCmdType into lobbyLastRejectPacket
 * and reasonCode into lobbyLastRejectReason. Drives all six in a single
 * test because the dispatcher arm treats them identically — pinning each
 * code independently catches an off-by-one in the assignment without
 * cluttering the harness with six near-identical entries. */
int run_command_rejected_parks_name_codes(void) {
    static const uint8_t k_codes[] = {
        (uint8_t)CMD_REJECT_NAME_EMPTY,
        (uint8_t)CMD_REJECT_NAME_RESERVED_PREFIX,
        (uint8_t)CMD_REJECT_NAME_RESERVED_SUFFIX,
        (uint8_t)CMD_REJECT_NAME_MIXED_SCRIPTS,
        (uint8_t)CMD_REJECT_NAME_INVALID,
        (uint8_t)CMD_REJECT_NAME_TAKEN,
    };
    const int n = (int)(sizeof(k_codes) / sizeof(k_codes[0]));

    for (int i = 0; i < n; i++) {
        ClientSim *cs = fresh_client_sim_as_slot(0);
        UT_ASSERT(cs != NULL);
        UT_ASSERT_MSG(clientSimGetLobbyLastRejectPacket(cs) == 0,
                      "fresh ClientSim must start with no pending reject");
        UT_ASSERT_MSG(clientSimGetLobbyLastRejectReason(cs) == 0,
                      "fresh ClientSim must start with no pending reason");

        ControlEvent evt;
        make_name_reject(&evt, /*origSlot=*/0, k_codes[i]);
        clientSimApplyControl(cs, &evt);

        UT_ASSERT_MSG(clientSimGetLobbyLastRejectPacket(cs)
                          == (uint8_t)CMD_NAME_CHANGE,
                      "reason=%u: expected lobbyLastRejectPacket=CMD_NAME_CHANGE,"
                      " got %u",
                      (unsigned)k_codes[i],
                      (unsigned)clientSimGetLobbyLastRejectPacket(cs));
        UT_ASSERT_MSG(clientSimGetLobbyLastRejectReason(cs) == k_codes[i],
                      "reason=%u: lobbyLastRejectReason did not round-trip"
                      " (got %u)",
                      (unsigned)k_codes[i],
                      (unsigned)clientSimGetLobbyLastRejectReason(cs));

        clientSimDestroy(cs);
    }
    return 0;
}

/* In-process subscribers (SP-host, bots) see every CTRL_COMMAND_REJECTED
 * the dispatcher publishes, not just their own. The arm filters by
 * origSlot == myPlayerNum so a reject attributed to another player's
 * command does not steal the local UI slot. */
int run_command_rejected_ignores_other_slot(void) {
    ClientSim *cs = fresh_client_sim_as_slot(2);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    make_name_reject(&evt, /*origSlot=*/5,
                     (uint8_t)CMD_REJECT_NAME_TAKEN);
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetLobbyLastRejectPacket(cs) == 0,
                  "reject for another slot must not park lobbyLastRejectPacket"
                  " (got %u)",
                  (unsigned)clientSimGetLobbyLastRejectPacket(cs));
    UT_ASSERT_MSG(clientSimGetLobbyLastRejectReason(cs) == 0,
                  "reject for another slot must not park lobbyLastRejectReason"
                  " (got %u)",
                  (unsigned)clientSimGetLobbyLastRejectReason(cs));

    clientSimDestroy(cs);
    return 0;
}

/* drainInGameNameReject (and the lobby toast) call
 * clientSimClearLobbyLastReject after surfacing so a return to lobby
 * doesn't re-show the same line. Pin the contract: clear wipes both
 * fields back to zero. */
int run_command_rejected_clear_resets_both_fields(void) {
    ClientSim *cs = fresh_client_sim_as_slot(0);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    make_name_reject(&evt, /*origSlot=*/0,
                     (uint8_t)CMD_REJECT_NAME_RESERVED_PREFIX);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetLobbyLastRejectPacket(cs) != 0);
    UT_ASSERT(clientSimGetLobbyLastRejectReason(cs) != 0);

    clientSimClearLobbyLastReject(cs);
    UT_ASSERT(clientSimGetLobbyLastRejectPacket(cs) == 0);
    UT_ASSERT(clientSimGetLobbyLastRejectReason(cs) == 0);

    clientSimDestroy(cs);
    return 0;
}

/*
 * Pins the slot-change bookkeeping in clientSimSetPlayerNum().
 *
 * The client owns exactly one tank and one lgm — its own — and both live at
 * the array index of its player slot, so a change of slot has to carry them
 * across. The server is free to hand back a different slot on a second
 * JOIN_ACCEPT: a mid-lobby PACKET_LOBBY_MAP_CHANGE sends the joiner through
 * JOIN_REQUEST -> JOIN_ACCEPT again.
 *
 * The move used to read its source from slot 0 unconditionally, which only
 * held for the first assignment. On any later one slot 0 was already NULL, so
 * the copy wrote NULL over the live pointers and dropped the only reference
 * to both objects. A fuzz run on the JOIN_ACCEPT path retained 17.5M chunks
 * (1.06 GB) over 20.5M accepts before hitting its memory cap.
 *
 * These tests assert the invariant directly rather than watching the heap:
 * across the whole array there is never more than one live tank and one live
 * lgm, and both sit at the current slot. An orphan left behind at the old
 * index shows up as a second live entry.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "test_harness.h"

/* Live entries across the whole array, so an orphan at a stale index counts. */
static int live_tanks(ClientSim *cs) {
    int n = 0, i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (clientSimGetGameSim(cs)->tanks[i] != NULL) n++;
    }
    return n;
}

static int live_lgmen(ClientSim *cs) {
    int n = 0, i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (clientSimGetGameSim(cs)->lgmen[i] != NULL) n++;
    }
    return n;
}

/* The two calls the JOIN_ACCEPT handler makes, in order, via
 * clientSimOnAssignedSlot. */
static void assign_slot(ClientSim *cs, BYTE slot) {
    clientSimSetPlayerNum(cs, slot);
    clientSimSetupSelf(cs, slot, "Joiner", 0, 0);
}

int run_client_slot_reassign_carries_tank_and_lgm(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimCreate(cs));

    assign_slot(cs, 3);
    UT_ASSERT_MSG(live_tanks(cs) == 1, "after first assignment: %d live tanks, expected 1",
                  live_tanks(cs));
    UT_ASSERT_MSG(live_lgmen(cs) == 1, "after first assignment: %d live lgmen, expected 1",
                  live_lgmen(cs));
    UT_ASSERT(clientSimGetGameSim(cs)->tanks[3] != NULL);

    /* Re-accept into a different slot. */
    assign_slot(cs, 7);
    UT_ASSERT_MSG(live_tanks(cs) == 1,
                  "slot 3 -> 7 left %d live tanks, expected 1 (orphan at the old slot)",
                  live_tanks(cs));
    UT_ASSERT_MSG(live_lgmen(cs) == 1,
                  "slot 3 -> 7 left %d live lgmen, expected 1 (orphan at the old slot)",
                  live_lgmen(cs));
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->tanks[3] == NULL, "old slot 3 still holds a tank");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->lgmen[3] == NULL, "old slot 3 still holds an lgm");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->tanks[7] != NULL, "new slot 7 has no tank");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->lgmen[7] != NULL, "new slot 7 has no lgm");
    UT_ASSERT(clientSimGetMyPlayerNum(cs) == 7);

    clientSimDestroy(cs);
    return 0;
}

int run_client_slot_reassign_same_slot_is_stable(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimCreate(cs));

    /* The server re-sending the same slot is the common re-accept, and it
     * must not disturb the pair already sitting there. */
    assign_slot(cs, 4);
    assign_slot(cs, 4);

    UT_ASSERT_MSG(live_tanks(cs) == 1, "repeat of slot 4 left %d live tanks, expected 1",
                  live_tanks(cs));
    UT_ASSERT_MSG(live_lgmen(cs) == 1, "repeat of slot 4 left %d live lgmen, expected 1",
                  live_lgmen(cs));
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->tanks[4] != NULL, "slot 4 lost its tank");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->lgmen[4] != NULL, "slot 4 lost its lgm");

    clientSimDestroy(cs);
    return 0;
}

int run_client_slot_reassign_back_to_zero(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimCreate(cs));

    /* Slot 0 is a legal re-assignment target, not a "no slot" marker. */
    assign_slot(cs, 5);
    assign_slot(cs, 0);

    UT_ASSERT_MSG(live_tanks(cs) == 1, "slot 5 -> 0 left %d live tanks, expected 1",
                  live_tanks(cs));
    UT_ASSERT_MSG(live_lgmen(cs) == 1, "slot 5 -> 0 left %d live lgmen, expected 1",
                  live_lgmen(cs));
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->tanks[5] == NULL, "old slot 5 still holds a tank");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->lgmen[5] == NULL, "old slot 5 still holds an lgm");
    UT_ASSERT_MSG(clientSimGetGameSim(cs)->tanks[0] != NULL, "slot 0 has no tank");
    UT_ASSERT(clientSimGetMyPlayerNum(cs) == 0);

    clientSimDestroy(cs);
    return 0;
}

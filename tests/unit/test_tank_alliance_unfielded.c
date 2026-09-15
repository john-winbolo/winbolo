/*
 * A seat with no tank shows no tank in the status strip.
 *
 * A horde seat between waves stays in the roster: unfielding keeps the
 * connection, the players-table identity and the name, so the table still
 * reads the seat as a live player. playersScreenAllience answers tankNone
 * only for a slot that is not inUse, so such a seat used to come back
 * tankEvil and draw the red enemy tile for a whole round with no tank
 * under it.
 *
 * clientSimGetTankAlliance is where the rule lives, because every per-frame
 * draw site goes through it (five in sdl3draw.c, one in the tablet panel).
 * playersSetPlayer works out its own alliance for the status tile on a join
 * or a rename, and asks the same predicate so the two cannot drift.
 *
 * The sprite blit itself is not testable here and is not tested; what is
 * pinned is the answer the blit is handed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"             /* clientSimGetTankAlliance, clientSimSlotIsUnfielded */
#include "client_sim_internal.h"    /* cs->lobbySlots */
#include "players.h"                /* playersSetPlayer */
#include "screentank.h"             /* tankAlliance */
#include "test_harness.h"

#define TA_SELF_SLOT   1  /* 0-based; the local player */
#define TA_RAIDER_SLOT 5  /* 0-based; a horde seat */

/* Register a slot in the players table so it reads as inUse, which is the
   whole of what playersScreenAllience looks at. Registered the way the
   server registers one — no ClientSim, isServer TRUE — so the front-end
   branch in playersSetPlayer stays out of it; this test is about the
   answer, not about the draw. */
static void taRegister(ClientSim *cs, BYTE slot, char *name) {
    playersSetPlayer(NULL, &clientSimGetGameSim(cs)->plyrs, NEUTRAL, slot,
                     name, "XX", 0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
}

int run_tank_alliance_unfielded_shows_no_tank(void) {
    ClientSim *cs = clientSimAlloc();
    char selfName[] = "Host";
    char raiderName[] = "Raider";

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, TA_SELF_SLOT);

    taRegister(cs, TA_SELF_SLOT, selfName);
    taRegister(cs, TA_RAIDER_SLOT, raiderName);

    /* The accessor takes a 1-based player number. Everything below reads
       slot + 1 for that reason; an off-by-one here would blank the wrong
       seat, which is exactly what the assertions are placed to catch. */

    cs->lobbySlots[TA_RAIDER_SLOT].connected = true;
    cs->lobbySlots[TA_RAIDER_SLOT].fielded   = true;
    UT_ASSERT_MSG(clientSimGetTankAlliance(cs, TA_RAIDER_SLOT + 1) == tankEvil,
                  "a fielded enemy seat keeps the answer it always had");

    /* The wave ends and the seat is held. */
    cs->lobbySlots[TA_RAIDER_SLOT].fielded = false;
    UT_ASSERT_MSG(clientSimGetTankAlliance(cs, TA_RAIDER_SLOT + 1) == tankNone,
                  "a held seat has no tank, so it reports none");

    /* The next wave fields it and the tile comes back. */
    cs->lobbySlots[TA_RAIDER_SLOT].fielded = true;
    UT_ASSERT_MSG(clientSimGetTankAlliance(cs, TA_RAIDER_SLOT + 1) == tankEvil,
                  "fielding the seat again restores its tile");

    /* Neither half of the test alone is the rule. A slot the roster is not
       holding is the players table's business as before — this is what
       keeps a departed player's still-inUse entry from going blank. */
    cs->lobbySlots[TA_RAIDER_SLOT].connected = false;
    cs->lobbySlots[TA_RAIDER_SLOT].fielded   = false;
    UT_ASSERT_MSG(clientSimGetTankAlliance(cs, TA_RAIDER_SLOT + 1) == tankEvil,
                  "an unconnected slot is not a held seat");

    /* The local player is answered from the table as before. */
    cs->lobbySlots[TA_SELF_SLOT].connected = true;
    cs->lobbySlots[TA_SELF_SLOT].fielded   = true;
    UT_ASSERT_MSG(clientSimGetTankAlliance(cs, TA_SELF_SLOT + 1) == tankSelf,
                  "the local player still reads as self");

    /* playersSetPlayer asks the predicate with no ClientSim on the server
       paths, where it must answer false and leave the tile alone. */
    UT_ASSERT_MSG(!clientSimSlotIsUnfielded(NULL, TA_RAIDER_SLOT),
                  "no ClientSim to ask means no seat is held");

    clientSimDestroy(cs);
    return 0;
}

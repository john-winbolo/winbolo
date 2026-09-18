/*
 * A seat with no tank shows no tank in the status strip.
 *
 * A held seat between waves stays in the roster: unfielding keeps the
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
 *
 * run_tank_alliance_unfielded_shows_no_tank  — the accessor every per-frame
 *                                              draw site reads
 * run_tank_alliance_unfielded_status_tile    — and the tile playersSetPlayer
 *                                              works out for itself on a join
 *                                              or a rename, read off the
 *                                              frontEndStatusTank stub
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
#define TA_RAIDER_SLOT 5  /* 0-based; a held seat */

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

/* The other route to the same rule. playersSetPlayer does not call the
 * accessor above: it works the alliance out from the table with the
 * selfPlayer it was handed, because a spectator arrives here with 0xFF and the
 * self branch must not swallow a real slot-0 join. So the held-seat rule has
 * to be applied on this route too, or a join or a rename writes the seat's old
 * tile into the status strip and it stands until something else redraws.
 *
 * Registered the way a client registers a remote player — a real ClientSim and
 * isServer FALSE — because the tile is only written on that branch. The case
 * above covers the other one, where csParam is NULL and the predicate answers
 * false. */
int run_tank_alliance_unfielded_status_tile(void) {
    ClientSim *cs = clientSimAlloc();
    char selfName[] = "Host";
    char raiderName[] = "Raider";

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, TA_SELF_SLOT);

    /* The local player, registered first so the table has an alliance list to
       ask about when it works the raider's tile out. */
    taRegister(cs, TA_SELF_SLOT, selfName);

    /* A seat the roster holds with nobody on the field: the wave that fielded
       it has ended. TA_RAIDER_SLOT is 0-based, as are the lobby mirror's index
       and the playerNum playersSetPlayer takes. */
    cs->lobbySlots[TA_RAIDER_SLOT].connected = true;
    cs->lobbySlots[TA_RAIDER_SLOT].fielded   = false;

    playersSetPlayer(cs, &clientSimGetGameSim(cs)->plyrs, TA_SELF_SLOT,
                     TA_RAIDER_SLOT, raiderName, "XX", 0, 0, 0, 0, 0,
                     FALSE, 0, NULL, FALSE);

    /* frontEndStatusTank takes a 1-based player number, so the slot the tile
       was written for reads TA_RAIDER_SLOT + 1. An off-by-one here would blank
       the wrong seat's tile, which is what this assertion is placed to
       catch. */
    UT_ASSERT_MSG(ut_status_tank_last_player() == TA_RAIDER_SLOT + 1,
                  "the status tile was written for player %d, expected %d — "
                  "the 1-based number for 0-based slot %d",
                  ut_status_tank_last_player(), TA_RAIDER_SLOT + 1,
                  TA_RAIDER_SLOT);
    UT_ASSERT_MSG(ut_status_tank_last_alliance() == (int)tankNone,
                  "a held seat's status tile was written as %d, expected "
                  "tankNone (%d) — the seat has no tank to draw",
                  ut_status_tank_last_alliance(), (int)tankNone);

    /* The next wave fields it and the tile is whatever the table says again. */
    cs->lobbySlots[TA_RAIDER_SLOT].fielded = true;
    playersSetPlayer(cs, &clientSimGetGameSim(cs)->plyrs, TA_SELF_SLOT,
                     TA_RAIDER_SLOT, raiderName, "XX", 0, 0, 0, 0, 0,
                     FALSE, 0, NULL, FALSE);

    UT_ASSERT_MSG(ut_status_tank_last_player() == TA_RAIDER_SLOT + 1,
                  "the second write went to player %d, expected %d",
                  ut_status_tank_last_player(), TA_RAIDER_SLOT + 1);
    UT_ASSERT_MSG(ut_status_tank_last_alliance() == (int)tankEvil,
                  "a fielded enemy seat's status tile was written as %d, "
                  "expected tankEvil (%d)", ut_status_tank_last_alliance(),
                  (int)tankEvil);

    clientSimDestroy(cs);
    return 0;
}

/*
 * Server-side LGM visibility (test_lgm_visibility.c).
 *
 * A man is a separate thing from the tank that sent him out, and he can be a
 * long way from it — parachuting in from a spawn, or off building. Two rules
 * hold whatever the owning tank is doing:
 *
 *   - a man on the helicopter frame (the parachute / respawn animation) is
 *     always sent;
 *   - a man who is not standing in trees is always sent.
 *
 * Tree hide applies to the tank, and serverSimBuildSnapshot ships a hidden tank
 * as a 1-byte TANK_SNAPSHOT_HIDDEN_FLAG stub — which carries no LGM fields
 * either, so hiding the tank used to take the man with it. These cases pin the
 * two rules to the built snapshot: the entry for a tank hidden in trees must
 * still arrive carrying its man, and must still not carry the tank's own
 * position (a modified client has to gain nothing from the entry existing).
 *
 * An entry sent for the man alone says so with TANK_HIDDEN_POSITION, which is
 * what tells the client to leave that tank out of interpolation rather than
 * read the zeroed fields as a position at the top corner of the map.
 *
 * Like test_tree_hide.c these drive ut_make_running_sim and poke the GameSim
 * directly (the unittests profile permits T2-internal access), asserting the
 * terrain preconditions first so a map or placement mistake reports itself
 * rather than showing up as a confusing stub/no-stub failure.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos — paint terrain */
#include "tank.h"                  /* tankSetWorld, MIN_TREEHIDE_DIST */
#include "lgm.h"                   /* LGM_HELICOPTER_FRAME, LGM_STATE_GOING */
#include "pillbox.h"
#include "bases.h"
#include "players.h"
#include "util.h"                  /* utilIsTankInTrees */
#include "input_packet.h"          /* TANK_SNAPSHOT_HIDDEN_FLAG */
#include "test_harness.h"

/* The world centre of a map square. */
static WORLD lv_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Hand every pill and base to nobody, so the recipient's rect set is just its
 * own tank screen. */
static void lv_clear_owners(GameSim *gs) {
    BYTE n;
    BYTE i;

    n = pillsGetNumPills(&gs->pb);
    for (i = 0; i < n; i++) {
        gs->pb->item[i].owner  = NEUTRAL;
        gs->pb->item[i].inTank = FALSE;
    }
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        gs->bs->item[i].owner = NEUTRAL;
    }
}

/* Whether (x,y) is inside the block lv_make_forest paints round (mx,my). */
static bool lv_in_block(BYTE x, BYTE y, BYTE mx, BYTE my) {
    int dx = (int)x - (int)mx;
    int dy = (int)y - (int)my;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 2 && dy <= 2;
}

/* utilIsTankInTrees also tests the squares next to the one the target stands on,
 * and a pillbox or base on any of them cancels the hide whoever owns it — so
 * paint a block rather than one square, and move anything standing in it away
 * first. */
static void lv_make_forest(GameSim *gs, BYTE mx, BYTE my) {
    int x;
    int y;
    BYTE n;
    BYTE i;

    n = pillsGetNumPills(&gs->pb);
    for (i = 0; i < n; i++) {
        if (lv_in_block(gs->pb->item[i].x, gs->pb->item[i].y, mx, my)) {
            gs->pb->item[i].x = (BYTE)(2 + i);
            gs->pb->item[i].y = 2;
        }
    }
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        if (lv_in_block(gs->bs->item[i].x, gs->bs->item[i].y, mx, my)) {
            gs->bs->item[i].x = (BYTE)(2 + i);
            gs->bs->item[i].y = 4;
        }
    }

    for (y = (int)my - 2; y <= (int)my + 2; y++) {
        for (x = (int)mx - 2; x <= (int)mx + 2; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, FOREST, FALSE, FALSE);
        }
    }
}

/* Open ground for a man to stand on, block-sized for the same reason. */
static void lv_make_grass(GameSim *gs, BYTE mx, BYTE my) {
    int x;
    int y;

    for (y = (int)my - 2; y <= (int)my + 2; y++) {
        for (x = (int)mx - 2; x <= (int)mx + 2; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, GRASS, FALSE, FALSE);
        }
    }
}

/* Park a live tank, unfired, on a map square. */
static void lv_place_tank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    tankSetWorld(gs, &gs->tanks[slot], lv_world(mx), lv_world(my), 0, false);
    gs->tanks[slot]->deathWait = 0;
    gs->tanks[slot]->justFired = 0;
}

/* Put a slot's man out of his tank on a map square, on the given frame.
 * lgmGetMX/MY back out the drawing offset the frame implies before reporting a
 * square, so seed x/y with that offset applied and the getters report (mx,my).
 * An idle man reports square 0 whatever his position, so the state has to move
 * off LGM_STATE_IDLE as well. */
static void lv_place_lgm(GameSim *gs, BYTE slot, BYTE mx, BYTE my, BYTE frame) {
    int offX = (frame == LGM_HELICOPTER_FRAME) ? MAP_SQUARE_MIDDLE : 1;
    int offY = (frame == LGM_HELICOPTER_FRAME) ? MAP_SQUARE_MIDDLE : 2;

    gs->lgmen[slot]->inTank = FALSE;
    gs->lgmen[slot]->state  = LGM_STATE_GOING;
    gs->lgmen[slot]->frame  = frame;
    gs->lgmen[slot]->x = (WORLD)(((int)mx << M_W_SHIFT_SIZE) + offX);
    gs->lgmen[slot]->y = (WORLD)(((int)my << M_W_SHIFT_SIZE) + offY);
}

/* One recipient's built tank block. The other collectors' output is not read by
 * these cases, so lv_build keeps those buffers to itself. */
typedef struct {
    SnapshotHeader hdr;
    TankSnapshot   tk[MAX_TANKS];
} LvSnap;

static void lv_build(ServerSim *sim, BYTE recipient, LvSnap *out) {
    ShellSnapshot sh[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot te[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot bo[MAX_SNAPSHOT_BASES];
    PillSnapshot po[MAX_SNAPSHOT_PILLS];
    GameEvent ev[MAX_SNAPSHOT_EVENTS];

    memset(out, 0, sizeof(*out));
    serverSimBuildSnapshot(sim, recipient, &out->hdr, out->tk, MAX_TANKS,
                           sh, MAX_SNAPSHOT_SHELLS,
                           te, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bo, MAX_SNAPSHOT_BASES,
                           po, MAX_SNAPSHOT_PILLS,
                           ev, MAX_SNAPSHOT_EVENTS, false);
}

/* The entry for one slot: a stub and a full entry both carry the slot in the
 * low 7 bits of playerNum, so the caller tests the flag itself. */
static const TankSnapshot *lv_entry(const LvSnap *snap, BYTE slot) {
    int i;

    for (i = 0; i < (int)snap->hdr.tankCount; i++) {
        if ((snap->tk[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK) == slot) {
            return &snap->tk[i];
        }
    }
    return NULL;
}

/* 1. An enemy tank hidden in trees ten squares away still sends its man when
 *    that man is on open ground or on the parachute — and in neither case
 *    gives away where the tank itself is. */
int run_lgm_visibility_tank_in_trees(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");
    UT_ASSERT_MSG(gs->lgmen[1] != NULL, "slot 1 needs a man for these cases");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim) == false,
                  "allies-in-trees must default off for these cases");

    lv_clear_owners(gs);
    lv_make_forest(gs, 60, 50);   /* the tank, ten squares from the recipient */
    lv_make_grass(gs, 54, 50);    /* the man, four squares away on open ground —
                                   * outside MIN_TREEHIDE_DIST, so his being
                                   * sent is the rule and not the near-range
                                   * exemption */
    lv_place_tank(gs, 0, 50, 50);
    lv_place_tank(gs, 1, 60, 50);

    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    lv_world(60), lv_world(50)) == TRUE,
                  "tank square (60,50) is not hidden by trees — the painted "
                  "forest block or the placement is wrong");

    LvSnap snap;
    const TankSnapshot *other;

    /* A man out building on open ground. */
    lv_place_lgm(gs, 1, 54, 50, 0);
    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    lv_world(54), lv_world(50)) == FALSE,
                  "man square (54,50) counts as trees — the open-ground case "
                  "would pass for the wrong reason");

    lv_build(sim, 0, &snap);
    other = lv_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL, "no entry for the enemy at ten squares");
    UT_ASSERT_MSG((other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a man on open ground must be sent even though his tank is "
                  "hidden in trees (playerNum 0x%02x)", other->playerNum);
    UT_ASSERT_MSG(other->lgmFrame != 0,
                  "the entry carries no man — lgmFrame is 0");
    UT_ASSERT_MSG(other->lgmMX == 54 && other->lgmMY == 50,
                  "the man is at the wrong square (%u,%u), wanted (54,50)",
                  other->lgmMX, other->lgmMY);
    UT_ASSERT_MSG(!(other->worldX == lv_world(60) &&
                    other->worldY == lv_world(50)),
                  "the entry gives away the hidden tank's position (%u,%u)",
                  other->worldX, other->worldY);
    UT_ASSERT_MSG((other->hiddenFlags & TANK_HIDDEN_POSITION) != 0,
                  "the entry must say the tank position is withheld so the "
                  "client leaves it out of interpolation (hiddenFlags 0x%02x)",
                  other->hiddenFlags);

    /* A man on the parachute, in the trees with his tank: the frame is sent
     * whatever he is standing on. */
    lv_place_lgm(gs, 1, 59, 49, LGM_HELICOPTER_FRAME);
    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    lv_world(59), lv_world(49)) == TRUE,
                  "parachute square (59,49) is not trees — the case would not "
                  "test the rule it means to");

    lv_build(sim, 0, &snap);
    other = lv_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL, "no entry for the enemy on the parachute case");
    UT_ASSERT_MSG((other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a parachuting man must be sent even in trees with his tank "
                  "hidden (playerNum 0x%02x)", other->playerNum);
    UT_ASSERT_MSG(other->lgmFrame == (BYTE)(LGM_HELICOPTER_FRAME + 1),
                  "wrong frame for the parachute: %u, wanted %u (the wire "
                  "carries frame+1)",
                  other->lgmFrame, (unsigned)(LGM_HELICOPTER_FRAME + 1));
    UT_ASSERT_MSG(other->lgmMX == 59 && other->lgmMY == 49,
                  "the parachute is at the wrong square (%u,%u), wanted "
                  "(59,49)", other->lgmMX, other->lgmMY);
    UT_ASSERT_MSG(!(other->worldX == lv_world(60) &&
                    other->worldY == lv_world(50)),
                  "the entry gives away the hidden tank's position (%u,%u)",
                  other->worldX, other->worldY);
    UT_ASSERT_MSG((other->hiddenFlags & TANK_HIDDEN_POSITION) != 0,
                  "the entry must say the tank position is withheld "
                  "(hiddenFlags 0x%02x)", other->hiddenFlags);

    /* A man in his tank is nothing to send, so the tank hide stands on its own
     * and the entry goes back to being a stub. */
    gs->lgmen[1]->inTank = TRUE;
    lv_build(sim, 0, &snap);
    other = lv_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL && (other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "with his man back in the tank a tank in trees must ship as a "
                  "stub (playerNum 0x%02x)",
                  other == NULL ? 0 : other->playerNum);

    serverSimDestroy(sim);
    return 0;
}

/* 2. The same two men, with the owning tank in the open: the entry arrives in
 *    full and carries both the tank and the man. This is the control for case
 *    1 — it fails only if something breaks the ordinary path. */
int run_lgm_visibility_tank_visible(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");
    UT_ASSERT_MSG(gs->lgmen[1] != NULL, "slot 1 needs a man for these cases");

    lv_clear_owners(gs);
    lv_make_grass(gs, 60, 50);    /* the tank, ten squares away in the open */
    lv_make_grass(gs, 54, 50);    /* the man, four squares away */
    lv_place_tank(gs, 0, 50, 50);
    lv_place_tank(gs, 1, 60, 50);

    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    lv_world(60), lv_world(50)) == FALSE,
                  "tank square (60,50) counts as trees — the control would "
                  "test the hidden path instead");

    LvSnap snap;
    const TankSnapshot *other;

    lv_place_lgm(gs, 1, 54, 50, 0);
    lv_build(sim, 0, &snap);
    other = lv_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL, "no entry for the enemy in the open");
    UT_ASSERT_MSG((other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a tank in the open must ship in full (playerNum 0x%02x)",
                  other->playerNum);
    UT_ASSERT_MSG(other->worldX == lv_world(60) && other->worldY == lv_world(50),
                  "the entry carries the wrong tank position (%u,%u)",
                  other->worldX, other->worldY);
    UT_ASSERT_MSG(other->hiddenFlags == 0,
                  "a tank in the open must not be marked withheld "
                  "(hiddenFlags 0x%02x)", other->hiddenFlags);
    UT_ASSERT_MSG(other->lgmMX == 54 && other->lgmMY == 50,
                  "the man is at the wrong square (%u,%u), wanted (54,50)",
                  other->lgmMX, other->lgmMY);

    lv_place_lgm(gs, 1, 54, 50, LGM_HELICOPTER_FRAME);
    lv_build(sim, 0, &snap);
    other = lv_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL &&
                  (other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a tank in the open must ship in full on the parachute case");
    UT_ASSERT_MSG(other->lgmFrame == (BYTE)(LGM_HELICOPTER_FRAME + 1),
                  "wrong frame for the parachute: %u, wanted %u",
                  other->lgmFrame, (unsigned)(LGM_HELICOPTER_FRAME + 1));

    serverSimDestroy(sim);
    return 0;
}

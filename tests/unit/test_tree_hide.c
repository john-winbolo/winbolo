/*
 * Server-side tree hide (test_tree_hide.c).
 *
 * A tank standing in trees is withheld from a recipient more than
 * MIN_TREEHIDE_DIST (768 world units, three map squares) away on either axis:
 * serverSimBuildSnapshot ships it as the same 1-byte
 * TANK_SNAPSHOT_HIDDEN_FLAG stub an out-of-view tank gets, so a modified
 * client has no position to draw. Two exemptions: a tank that has just fired
 * gives its position away anyway (the shot clears the trees it was hiding in),
 * and an ally is sent in full while the server's allies-in-trees option is on.
 *
 * Both cases drive ut_make_running_sim, poke the GameSim directly (the
 * unittests profile permits T2-internal access) and read the tank entries back
 * out of a built snapshot. Each case asserts utilIsTankInTrees agrees the
 * target is hidden before asserting anything about the snapshot, so a map or
 * geometry mistake reports itself rather than showing up as a confusing
 * stub/no-stub failure.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos — paint the forest block */
#include "tank.h"                  /* tankSetWorld, MIN_TREEHIDE_DIST */
#include "pillbox.h"
#include "bases.h"
#include "players.h"               /* playersAcceptAlliance, playersIsAllie */
#include "util.h"                  /* utilIsTankInTrees */
#include "input_packet.h"          /* TANK_SNAPSHOT_HIDDEN_FLAG */
#include "test_harness.h"

/* The world centre of a map square. */
static WORLD th_world(BYTE mapCoord) {
    return (WORLD)(((int)mapCoord << M_W_SHIFT_SIZE) + MAP_SQUARE_MIDDLE);
}

/* Hand every pill and base to nobody, so the recipient's rect set is just its
 * own tank screen plus whatever a case's alliance adds. */
static void th_clear_owners(GameSim *gs) {
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

/* Park a live tank, unfired, on a map square. */
static void th_place_tank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    tankSetWorld(gs, &gs->tanks[slot], th_world(mx), th_world(my), 0, false);
    gs->tanks[slot]->deathWait = 0;
    gs->tanks[slot]->justFired = 0;
}

/* Whether (x,y) is inside the block th_make_forest paints round (mx,my). */
static bool th_in_block(BYTE x, BYTE y, BYTE mx, BYTE my) {
    int dx = (int)x - (int)mx;
    int dy = (int)y - (int)my;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 2 && dy <= 2;
}

/* utilIsTankInTrees also tests the squares next to the one the tank stands on
 * (which ones depends on the sub-square position), and a pillbox or base on
 * any of them cancels the hide whoever owns it — so paint a block rather than
 * one square, and move any pill or base standing in it out of the way first. */
static void th_make_forest(GameSim *gs, BYTE mx, BYTE my) {
    int x;
    int y;
    BYTE n;
    BYTE i;

    n = pillsGetNumPills(&gs->pb);
    for (i = 0; i < n; i++) {
        if (th_in_block(gs->pb->item[i].x, gs->pb->item[i].y, mx, my)) {
            gs->pb->item[i].x = (BYTE)(2 + i);
            gs->pb->item[i].y = 2;
        }
    }
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        if (th_in_block(gs->bs->item[i].x, gs->bs->item[i].y, mx, my)) {
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

/* One recipient's built tank block. The other collectors' output is not read
 * by these cases, so th_build keeps those buffers to itself. */
typedef struct {
    SnapshotHeader hdr;
    TankSnapshot   tk[MAX_TANKS];
} ThSnap;

static void th_build(ServerSim *sim, BYTE recipient, ThSnap *out) {
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
static const TankSnapshot *th_entry(const ThSnap *snap, BYTE slot) {
    int i;

    for (i = 0; i < (int)snap->hdr.tankCount; i++) {
        if ((snap->tk[i].playerNum & TANK_SNAPSHOT_PLAYER_MASK) == slot) {
            return &snap->tk[i];
        }
    }
    return NULL;
}

/* 1. An enemy in trees ten squares away arrives as a stub; the same tank two
 *    squares away arrives in full; just after firing it arrives in full at ten
 *    squares. The recipient's own entry is never a stub. */
int run_tree_hide_enemy(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL,
                  "slots 0/1 need tanks for positioning");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) != TRUE,
                  "players 0 and 1 allied by default — breaks the enemy case");
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim) == false,
                  "allies-in-trees must default off for this case");

    th_clear_owners(gs);
    th_make_forest(gs, 60, 50);   /* ten squares from the recipient */
    th_make_forest(gs, 52, 50);   /* two squares — reaches the recipient's own
                                   * square, which does not matter: the
                                   * recipient's tank is never culled */
    th_place_tank(gs, 0, 50, 50);
    th_place_tank(gs, 1, 60, 50);

    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    th_world(60), th_world(50)) == TRUE,
                  "target square (60,50) is not hidden by trees — the painted "
                  "forest block or the placement is wrong");

    ThSnap snap;
    const TankSnapshot *self;
    const TankSnapshot *other;

    th_build(sim, 0, &snap);
    other = th_entry(&snap, 1);
    self  = th_entry(&snap, 0);
    UT_ASSERT_MSG(other != NULL, "no entry for the enemy at ten squares");
    UT_ASSERT_MSG((other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "enemy in trees ten squares away must ship as a stub "
                  "(playerNum 0x%02x)", other->playerNum);
    UT_ASSERT_MSG(self != NULL &&
                  (self->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "the recipient's own tank must never be stubbed");

    /* Firing gives the position away: the shot clears the cover. */
    gs->tanks[1]->justFired = 1;
    th_build(sim, 0, &snap);
    other = th_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL &&
                  (other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a tank that has just fired must ship in full even in trees "
                  "(playerNum 0x%02x)", other == NULL ? 0 : other->playerNum);
    UT_ASSERT_MSG(other->worldX == th_world(60) && other->worldY == th_world(50),
                  "just-fired entry carries the wrong position (%u,%u)",
                  other->worldX, other->worldY);

    /* Cleared again, the hide comes back. */
    gs->tanks[1]->justFired = 0;
    th_build(sim, 0, &snap);
    other = th_entry(&snap, 1);
    UT_ASSERT_MSG(other != NULL &&
                  (other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "the hide must return once justFired has run out");

    /* Inside MIN_TREEHIDE_DIST on both axes there is no hide. */
    th_place_tank(gs, 1, 52, 50);
    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    th_world(52), th_world(50)) == TRUE,
                  "near square (52,50) is not hidden by trees — the near case "
                  "would pass for the wrong reason");
    th_build(sim, 0, &snap);
    other = th_entry(&snap, 1);
    self  = th_entry(&snap, 0);
    UT_ASSERT_MSG(other != NULL &&
                  (other->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "a tank in trees two squares away must ship in full "
                  "(playerNum 0x%02x)", other == NULL ? 0 : other->playerNum);
    UT_ASSERT_MSG(self != NULL &&
                  (self->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "the recipient's own tank must never be stubbed");

    serverSimDestroy(sim);
    return 0;
}

/* 2. An ally in trees ten squares away is a stub with the option off and
 *    arrives in full with it on; an enemy in trees is a stub either way. */
int run_tree_hide_ally_option(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL && gs->tanks[1] != NULL &&
                  gs->tanks[2] != NULL,
                  "slots 0/1/2 need tanks for positioning");

    th_clear_owners(gs);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, 0, 1, TRUE);
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 1) == TRUE,
                  "players 0 and 1 should be allied for this case");
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 0, 2) != TRUE,
                  "players 0 and 2 must stay un-allied for the enemy arm");

    th_make_forest(gs, 60, 50);   /* the ally, ten squares away */
    th_make_forest(gs, 60, 60);   /* the enemy, ten squares on both axes */
    th_place_tank(gs, 0, 50, 50);
    th_place_tank(gs, 1, 60, 50);
    th_place_tank(gs, 2, 60, 60);

    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    th_world(60), th_world(50)) == TRUE,
                  "ally square (60,50) is not hidden by trees");
    UT_ASSERT_MSG(utilIsTankInTrees(&gs->mp, &gs->pb, &gs->bs,
                                    th_world(60), th_world(60)) == TRUE,
                  "enemy square (60,60) is not hidden by trees");

    ThSnap snap;
    const TankSnapshot *ally;
    const TankSnapshot *enemy;

    /* Option off is the classic rule: an ally in trees is not sent either. */
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim) == false,
                  "allies-in-trees must default off");
    th_build(sim, 0, &snap);
    ally  = th_entry(&snap, 1);
    enemy = th_entry(&snap, 2);
    UT_ASSERT_MSG(ally != NULL &&
                  (ally->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "with the option off an ally in trees must ship as a stub "
                  "(playerNum 0x%02x)", ally == NULL ? 0 : ally->playerNum);
    UT_ASSERT_MSG(enemy != NULL &&
                  (enemy->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "an enemy in trees must ship as a stub with the option off");

    /* Option on: the ally arrives in full, the enemy still does not. */
    serverSimSetAlliesInTrees(sim, true);
    UT_ASSERT_MSG(serverSimGetAlliesInTrees(sim) == true,
                  "serverSimSetAlliesInTrees did not take");
    th_build(sim, 0, &snap);
    ally  = th_entry(&snap, 1);
    enemy = th_entry(&snap, 2);
    UT_ASSERT_MSG(ally != NULL &&
                  (ally->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) == 0,
                  "with the option on an ally in trees must ship in full "
                  "(playerNum 0x%02x)", ally == NULL ? 0 : ally->playerNum);
    UT_ASSERT_MSG(ally->worldX == th_world(60) && ally->worldY == th_world(50),
                  "the ally's entry carries the wrong position (%u,%u)",
                  ally->worldX, ally->worldY);
    UT_ASSERT_MSG(enemy != NULL &&
                  (enemy->playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) != 0,
                  "the option must not reveal an enemy in trees "
                  "(playerNum 0x%02x)", enemy == NULL ? 0 : enemy->playerNum);

    serverSimDestroy(sim);
    return 0;
}

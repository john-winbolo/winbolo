/*
 * The six entity ops a scenario changes the map's item lists with.
 *
 * Adding and removing a pillbox, a base or a start is the one op group that
 * changes how long a list is mid-round. Removal is a tombstone — the slot, the
 * count and every index above it stay put, because those numbers are what the
 * snapshots, the recordings and the brain API use to name an item — and an add
 * takes the lowest removed slot before it extends the list. The list chooses
 * the slot and reports it, so every add case here reads out->index rather than
 * assuming one.
 *
 * Each case asks three things of its arm: does every refusal in its contract
 * come back with its own code, does the mutation land on the sim, and does the
 * change reach the bus as a CTRL_ENTITY_CHANGE.
 *
 * Item indices are 0-based in an op and 1-based in the three list modules, so
 * every arm that takes an index is driven at index 0 and at the last index.
 *
 * Everard Island is the map behind ut_make_running_sim and it carries 11
 * bases, 16 pillboxes and 16 starts. Two of the three lists are therefore
 * already at MAX_*, so a pillbox or start add is staged by removing an item
 * first and adding into the slot that frees — which is also how the reuse case
 * is covered. The base list has room, so its add appends and its full case
 * fills the list first.
 *
 * run_scenario_entity_add_pill      — the add, its slot reuse, its refusals
 * run_scenario_entity_remove_pill   — the tombstone, index 0 and the last one
 * run_scenario_entity_add_base      — the append, and the full list
 * run_scenario_entity_remove_base   — the tombstone and its refusals
 * run_scenario_entity_add_start     — the add onto open water
 * run_scenario_entity_remove_start  — the tombstone, and the last start
 * run_scenario_entity_publish       — what reaches the bus, all three kinds
 * run_scenario_entity_add_out_null  — a successful add with nowhere to report
 *
 * Drives serverSimApplyScenarioOp at ut_make_running_sim and reads the
 * ServerSim struct directly; the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, sim.pb / .bs / .ss */
#include "server_sim_scenario.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapGetPos / mapSetPos, mapIsMine */
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "mines.h"                 /* minesExistPos — the empty-square scan */
#include "control_event.h"
#include "test_harness.h"

/* ── Small helpers ───────────────────────────────────────────────── */

/* Apply one op and hand back what the funnel answered. */
static ScnOpResult eaApply(ServerSim *sim, const ScenarioOp *op,
                           ScnOpOut *out) {
    return serverSimApplyScenarioOp(sim, op, out);
}

/* An empty land square a pillbox or a base could stand on: inside the minable
   area, nothing already on it, and ground that is not water or a structure.
   This is the arm's own test read back independently, so a square this finds
   is one the arm must accept. */
static bool eaFindLand(ServerSim *sim, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            BYTE t = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);
            if (t != GRASS && t != ROAD && t != SWAMP && t != RUBBLE &&
                t != CRATER && t != FOREST) {
                continue;
            }
            if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) continue;
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) continue;
            if (minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y)) continue;
            *ox = (BYTE)x;
            *oy = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A square a start can name: open water with no mine on it, inside the
   minable area. */
static bool eaFindDeepSea(ServerSim *sim, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == DEEP_SEA &&
                mapIsMine(&gs->mp, (BYTE)x, (BYTE)y) == FALSE) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Fill in an add-pill op. */
static void eaAddPillOp(ScenarioOp *op, BYTE x, BYTE y, BYTE owner,
                        BYTE armour, BYTE speed) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ENTITY_ADD_PILL;
    op->u.entityAddPill.x = x;
    op->u.entityAddPill.y = y;
    op->u.entityAddPill.owner = owner;
    op->u.entityAddPill.armour = armour;
    op->u.entityAddPill.speed = speed;
}

static void eaAddBaseOp(ScenarioOp *op, BYTE x, BYTE y, BYTE owner,
                        BYTE armour, BYTE shells, BYTE mines) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ENTITY_ADD_BASE;
    op->u.entityAddBase.x = x;
    op->u.entityAddBase.y = y;
    op->u.entityAddBase.owner = owner;
    op->u.entityAddBase.armour = armour;
    op->u.entityAddBase.shells = shells;
    op->u.entityAddBase.mines = mines;
}

static void eaAddStartOp(ScenarioOp *op, BYTE x, BYTE y, BYTE dir) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ENTITY_ADD_START;
    op->u.entityAddStart.x = x;
    op->u.entityAddStart.y = y;
    op->u.entityAddStart.dir = dir;
}

static void eaRemoveOp(ScenarioOp *op, ScenarioOpType type, BYTE index) {
    memset(op, 0, sizeof(*op));
    op->type = type;
    if (type == SCN_OP_ENTITY_REMOVE_PILL) {
        op->u.entityRemovePill.pill = index;
    } else if (type == SCN_OP_ENTITY_REMOVE_BASE) {
        op->u.entityRemoveBase.base = index;
    } else {
        op->u.entityRemoveStart.start = index;
    }
}

/* ── Pill: add ───────────────────────────────────────────────────── */

int run_scenario_entity_add_pill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    ScnOpOut out;
    pillbox got;
    BYTE lx = 0, ly = 0;
    BYTE before;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    before = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(before == MAX_PILLS,
                  "Everard Island should fill the pill list, has %u",
                  (unsigned)before);

    /* Full: every slot in the count is live. */
    UT_ASSERT(eaFindLand(sim, &lx, &ly));
    eaAddPillOp(&op, lx, ly, NEUTRAL, PILLS_MAX_ARMOUR, PILLBOX_ATTACK_NORMAL);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_FULL,
                  "a list of 16 live pillboxes should refuse an add");

    /* Free slot 2 (index 1) so the rest of the case has somewhere to land.
       Staged through the list rather than the op, so this case turns on the
       add alone. */
    UT_ASSERT(pillsRemoveItem(&gs->pb, 2) == TRUE);

    /* Off the map. */
    eaAddPillOp(&op, 0, 0, NEUTRAL, PILLS_MAX_ARMOUR, PILLBOX_ATTACK_NORMAL);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_SQUARE,
                  "square 0,0 is outside the minable area");

    /* An owner that is not a seat and not nobody. */
    eaAddPillOp(&op, lx, ly, (BYTE)(MAX_TANKS + 1), PILLS_MAX_ARMOUR,
                PILLBOX_ATTACK_NORMAL);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "an owner past the roster should be refused");

    /* Armour past what a pillbox can hold. */
    eaAddPillOp(&op, lx, ly, NEUTRAL, (BYTE)(PILLS_MAX_ARMOUR + 1),
                PILLBOX_ATTACK_NORMAL);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "armour past the cap should be refused, not clamped");

    /* An attack interval outside the pair. */
    eaAddPillOp(&op, lx, ly, NEUTRAL, PILLS_MAX_ARMOUR,
                (BYTE)(PILLBOX_MAX_FIRERATE - 1));
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "a speed under the fastest rate should be refused");

    /* Ground that will not take one: the square another pillbox is on. */
    {
        pillbox sitting;
        memset(&sitting, 0, sizeof(sitting));
        pillsGetPill(&gs->pb, &sitting, 1);
        eaAddPillOp(&op, sitting.x, sitting.y, NEUTRAL, PILLS_MAX_ARMOUR,
                    PILLBOX_ATTACK_NORMAL);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "a square already holding a pillbox should be refused");
    }

    /* Deep sea is ground a pillbox cannot stand on. */
    {
        BYTE sx = 0, sy = 0;
        UT_ASSERT(eaFindDeepSea(sim, &sx, &sy));
        eaAddPillOp(&op, sx, sy, NEUTRAL, PILLS_MAX_ARMOUR,
                    PILLBOX_ATTACK_NORMAL);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "open water should not take a pillbox");
    }

    /* Nothing above has changed the list. */
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == before,
                  "a refused add changed the count to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));

    /* The add itself: it lands in the freed slot rather than extending the
       list, which is the whole point of the tombstone. */
    memset(&out, 0, sizeof(out));
    out.index = 0xFF;
    eaAddPillOp(&op, lx, ly, 3, 7, 40);
    UT_ASSERT_MSG(eaApply(sim, &op, &out) == SCN_OP_OK,
                  "the add was refused on a square eaFindLand accepted");
    UT_ASSERT_MSG(out.index == 1,
                  "the add reported index %u, expected the freed slot 1",
                  (unsigned)out.index);
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == before,
                  "reusing a slot changed the count to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 2) == TRUE,
                  "the added pillbox is not on the map");

    memset(&got, 0, sizeof(got));
    pillsGetPill(&gs->pb, &got, 2);
    UT_ASSERT_MSG(got.x == lx && got.y == ly,
                  "the pillbox landed at %u,%u, wanted %u,%u",
                  (unsigned)got.x, (unsigned)got.y, (unsigned)lx,
                  (unsigned)ly);
    UT_ASSERT_MSG(got.owner == 3, "owner became %u", (unsigned)got.owner);
    UT_ASSERT_MSG(got.armour == 7, "armour became %u", (unsigned)got.armour);
    UT_ASSERT_MSG(got.speed == 40, "speed became %u", (unsigned)got.speed);
    UT_ASSERT_MSG(got.inTank == FALSE,
                  "a pillbox put on the map should not be in a tank");

    serverSimDestroy(sim);
    return 0;
}

/* ── Pill: remove ────────────────────────────────────────────────── */

int run_scenario_entity_remove_pill(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    BYTE count;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    count = pillsGetNumPills(&gs->pb);
    UT_ASSERT(count >= 2);

    /* Past the end of the list. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, count);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "an index past the count should be refused");

    /* Index 0. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing pillbox index 0 was refused");
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 1) == FALSE,
                  "pillbox 1 survived its removal");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == count,
                  "a removal changed the count to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));

    /* Already off the map reads the same as out of range. */
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "a second removal of the same index should be refused");

    /* The last index. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, (BYTE)(count - 1));
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing the last pillbox index was refused");
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, count) == FALSE,
                  "the last pillbox survived its removal");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == count,
                  "removing the top index shortened the list");

    /* Everything between the two is untouched. */
    {
        BYTE i;
        for (i = 2; i < count; i++) {
            UT_ASSERT_MSG(pillsIsActive(&gs->pb, i) == TRUE,
                          "pillbox %u went with its neighbours", (unsigned)i);
        }
    }

    /* A pillbox in a tank is on that tank's carry list and cannot be taken
       off the map from under it. */
    {
        pillbox carried;
        memset(&carried, 0, sizeof(carried));
        pillsGetPill(&gs->pb, &carried, 3);
        carried.inTank = TRUE;
        pillsSetPill(gs, &gs->pb, &carried, 3);
        eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 2);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_CARRIED,
                      "a carried pillbox should be refused");
        UT_ASSERT_MSG(pillsIsActive(&gs->pb, 3) == TRUE,
                      "a refused removal took the pillbox anyway");
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Base: add ───────────────────────────────────────────────────── */

int run_scenario_entity_add_base(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    ScnOpOut out;
    base got;
    BYTE lx = 0, ly = 0;
    BYTE before;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    before = basesGetNumBases(&gs->bs);
    UT_ASSERT_MSG(before > 0 && before < MAX_BASES,
                  "Everard Island should leave room in the base list, has %u",
                  (unsigned)before);
    UT_ASSERT(eaFindLand(sim, &lx, &ly));

    /* Off the map. */
    eaAddBaseOp(&op, 0, 0, NEUTRAL, BASE_FULL_ARMOUR, BASE_FULL_SHELLS,
                BASE_FULL_MINES);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_SQUARE,
                  "square 0,0 is outside the minable area");

    /* An owner that is neither a seat nor nobody. */
    eaAddBaseOp(&op, lx, ly, (BYTE)(MAX_TANKS + 1), 0, 0, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "an owner past the roster should be refused");

    /* A stock past a full load. */
    eaAddBaseOp(&op, lx, ly, NEUTRAL, BASE_FULL_ARMOUR,
                (BYTE)(BASE_FULL_SHELLS + 1), BASE_FULL_MINES);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "shells past a full load should be refused, not clamped");

    /* Open water. */
    {
        BYTE sx = 0, sy = 0;
        UT_ASSERT(eaFindDeepSea(sim, &sx, &sy));
        eaAddBaseOp(&op, sx, sy, NEUTRAL, 0, 0, 0);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "open water should not take a base");
    }

    /* A square a pillbox is standing on. */
    {
        pillbox sitting;
        memset(&sitting, 0, sizeof(sitting));
        pillsGetPill(&gs->pb, &sitting, 1);
        eaAddBaseOp(&op, sitting.x, sitting.y, NEUTRAL, 0, 0, 0);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "a square holding a pillbox should not take a base");
    }

    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == before,
                  "a refused add changed the count to %u",
                  (unsigned)basesGetNumBases(&gs->bs));

    /* The add: nothing is removed, so it extends the list and the index it
       reports is the new top one. */
    memset(&out, 0, sizeof(out));
    out.index = 0xFF;
    eaAddBaseOp(&op, lx, ly, 2, 11, 22, 33);
    UT_ASSERT_MSG(eaApply(sim, &op, &out) == SCN_OP_OK,
                  "the add was refused on a square eaFindLand accepted");
    UT_ASSERT_MSG(out.index == before,
                  "the add reported index %u, expected the appended slot %u",
                  (unsigned)out.index, (unsigned)before);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == (BYTE)(before + 1),
                  "the count is %u after an append",
                  (unsigned)basesGetNumBases(&gs->bs));
    UT_ASSERT(basesIsActive(&gs->bs, (BYTE)(before + 1)) == TRUE);

    memset(&got, 0, sizeof(got));
    basesGetBase(&gs->bs, &got, (BYTE)(before + 1));
    UT_ASSERT_MSG(got.x == lx && got.y == ly,
                  "the base landed at %u,%u, wanted %u,%u",
                  (unsigned)got.x, (unsigned)got.y, (unsigned)lx,
                  (unsigned)ly);
    UT_ASSERT_MSG(got.owner == 2, "owner became %u", (unsigned)got.owner);
    UT_ASSERT_MSG(got.armour == 11, "armour became %u", (unsigned)got.armour);
    UT_ASSERT_MSG(got.shells == 22, "shells became %u", (unsigned)got.shells);
    UT_ASSERT_MSG(got.mines == 33, "mines became %u", (unsigned)got.mines);

    /* Fill the rest of the list, then the next one is refused. */
    while (basesGetNumBases(&gs->bs) < MAX_BASES) {
        BYTE nx = 0, ny = 0;
        UT_ASSERT(eaFindLand(sim, &nx, &ny));
        eaAddBaseOp(&op, nx, ny, NEUTRAL, 0, 0, 0);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                      "filling the base list was refused at %u",
                      (unsigned)basesGetNumBases(&gs->bs));
    }
    {
        BYTE nx = 0, ny = 0;
        UT_ASSERT(eaFindLand(sim, &nx, &ny));
        eaAddBaseOp(&op, nx, ny, NEUTRAL, 0, 0, 0);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_FULL,
                      "a list of 16 live bases should refuse an add");
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Base: remove ────────────────────────────────────────────────── */

int run_scenario_entity_remove_base(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    BYTE count;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    count = basesGetNumBases(&gs->bs);
    UT_ASSERT(count >= 2);

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, count);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "an index past the count should be refused");

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing base index 0 was refused");
    UT_ASSERT(basesIsActive(&gs->bs, 1) == FALSE);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == count,
                  "a removal changed the count to %u",
                  (unsigned)basesGetNumBases(&gs->bs));

    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "a second removal of the same index should be refused");

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, (BYTE)(count - 1));
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing the last base index was refused");
    UT_ASSERT(basesIsActive(&gs->bs, count) == FALSE);
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == count,
                  "removing the top index shortened the list");

    {
        BYTE i;
        for (i = 2; i < count; i++) {
            UT_ASSERT_MSG(basesIsActive(&gs->bs, i) == TRUE,
                          "base %u went with its neighbours", (unsigned)i);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Start: add ──────────────────────────────────────────────────── */

int run_scenario_entity_add_start(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    ScnOpOut out;
    start got;
    BYTE sx = 0, sy = 0;
    BYTE before;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    before = startsGetNumStarts(&gs->ss);
    UT_ASSERT_MSG(before == MAX_STARTS,
                  "Everard Island should fill the start list, has %u",
                  (unsigned)before);
    UT_ASSERT(eaFindDeepSea(sim, &sx, &sy));

    /* Full. */
    eaAddStartOp(&op, sx, sy, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_FULL,
                  "a list of 16 live starts should refuse an add");

    UT_ASSERT(startsRemoveItem(&gs->ss, 4) == TRUE);

    /* Off the map. */
    eaAddStartOp(&op, 0, 0, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_SQUARE,
                  "square 0,0 is outside the minable area");

    /* A direction past the sixteen. */
    eaAddStartOp(&op, sx, sy, 16);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "a direction past 15 should be refused, not folded to 0");

    /* Land. A start names the water a tank arrives on, so ground is the
       wrong kind of square for one — the reverse of the pillbox rule. */
    {
        BYTE lx = 0, ly = 0;
        UT_ASSERT(eaFindLand(sim, &lx, &ly));
        eaAddStartOp(&op, lx, ly, 0);
        UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                      "a start on land should be refused");
    }

    /* The add lands in the freed slot. */
    memset(&out, 0, sizeof(out));
    out.index = 0xFF;
    eaAddStartOp(&op, sx, sy, 9);
    UT_ASSERT_MSG(eaApply(sim, &op, &out) == SCN_OP_OK,
                  "the add was refused on a deep-sea square");
    UT_ASSERT_MSG(out.index == 3,
                  "the add reported index %u, expected the freed slot 3",
                  (unsigned)out.index);
    UT_ASSERT_MSG(startsGetNumStarts(&gs->ss) == before,
                  "reusing a slot changed the count to %u",
                  (unsigned)startsGetNumStarts(&gs->ss));
    UT_ASSERT(startsIsActive(&gs->ss, 4) == TRUE);

    memset(&got, 0, sizeof(got));
    startsGetStartStruct(&gs->ss, &got, 4);
    UT_ASSERT_MSG(got.x == sx && got.y == sy,
                  "the start landed at %u,%u, wanted %u,%u",
                  (unsigned)got.x, (unsigned)got.y, (unsigned)sx,
                  (unsigned)sy);
    UT_ASSERT_MSG(got.dir == 9, "dir became %u", (unsigned)got.dir);

    serverSimDestroy(sim);
    return 0;
}

/* ── Start: remove ───────────────────────────────────────────────── */

int run_scenario_entity_remove_start(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    BYTE count;
    BYTE i;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    count = startsGetNumStarts(&gs->ss);
    UT_ASSERT(count >= 3);

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, count);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "an index past the count should be refused");

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, 0);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing start index 0 was refused");
    UT_ASSERT(startsIsActive(&gs->ss, 1) == FALSE);
    UT_ASSERT_MSG(startsGetNumStarts(&gs->ss) == count,
                  "a removal changed the count to %u",
                  (unsigned)startsGetNumStarts(&gs->ss));

    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "a second removal of the same index should be refused");

    /* Take the list down to two live starts, staged through the list so the
       case turns on the arm's own rule rather than on fifteen op calls. */
    for (i = 2; i <= (BYTE)(count - 2); i++) {
        if (startsIsActive(&gs->ss, i)) {
            UT_ASSERT(startsRemoveItem(&gs->ss, i) == TRUE);
        }
    }

    /* Two left: the second to last goes. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, (BYTE)(count - 2));
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "removing the second-to-last start was refused");

    /* One left: it stays, whatever the script asks. A map with nowhere to
       put a tank is not a state the sim can be left in. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, (BYTE)(count - 1));
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_RANGE,
                  "removing the last live start should be refused");
    UT_ASSERT_MSG(startsIsActive(&gs->ss, count) == TRUE,
                  "the last start went anyway");

    serverSimDestroy(sim);
    return 0;
}

/* ── What reaches the bus ────────────────────────────────────────── */

typedef struct {
    int     count;
    uint8_t kind;
    uint8_t index;
    uint8_t added;
    uint8_t recX;
    uint8_t recY;
} EntityWatch;

static void eaWatchDeliver(void *ctx, const struct ControlEvent *evt) {
    EntityWatch *w = (EntityWatch *)ctx;
    if (evt->type != CTRL_ENTITY_CHANGE) {
        return;
    }
    w->count++;
    w->kind  = evt->u.entityChange.kind;
    w->index = evt->u.entityChange.index;
    w->added = evt->u.entityChange.added;
    switch (evt->u.entityChange.kind) {
    case ENTITY_KIND_PILL:
        w->recX = evt->u.entityChange.rec.pill.x;
        w->recY = evt->u.entityChange.rec.pill.y;
        break;
    case ENTITY_KIND_BASE:
        w->recX = evt->u.entityChange.rec.base.x;
        w->recY = evt->u.entityChange.rec.base.y;
        break;
    default:
        w->recX = evt->u.entityChange.rec.start.x;
        w->recY = evt->u.entityChange.rec.start.y;
        break;
    }
}

/* Every arm that changes a list says so on the bus, with the index the list
 * chose and the record as it stands. A refused op says nothing. */
int run_scenario_entity_publish(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    ScnOpOut out;
    EntityWatch w;
    SubscriberHandle h;
    BYTE lx = 0, ly = 0, sx = 0, sy = 0;
    BYTE baseCount;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    UT_ASSERT(eaFindLand(sim, &lx, &ly));
    UT_ASSERT(eaFindDeepSea(sim, &sx, &sy));
    baseCount = basesGetNumBases(&gs->bs);

    memset(&w, 0, sizeof(w));
    h = serverSimRegisterSubscriber(sim, eaWatchDeliver, &w);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    /* The sync replay the registration runs carries no entity change. */
    UT_ASSERT_MSG(w.count == 0,
                  "the join replay published %d entity changes", w.count);

    /* A refused op is silent. */
    eaAddPillOp(&op, 0, 0, NEUTRAL, PILLS_MAX_ARMOUR, PILLBOX_ATTACK_NORMAL);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    UT_ASSERT_MSG(w.count == 0, "a refused add published %d events", w.count);

    /* A pillbox removal. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 5);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 1, "the removal published %d events", w.count);
    UT_ASSERT_MSG(w.kind == ENTITY_KIND_PILL, "kind %u", (unsigned)w.kind);
    UT_ASSERT_MSG(w.index == 5, "index %u", (unsigned)w.index);
    UT_ASSERT_MSG(w.added == 0, "a removal said added=%u", (unsigned)w.added);

    /* And the add back into the slot it freed. */
    memset(&out, 0, sizeof(out));
    eaAddPillOp(&op, lx, ly, NEUTRAL, 4, 60);
    UT_ASSERT(eaApply(sim, &op, &out) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 2, "the add published %d events", w.count);
    UT_ASSERT_MSG(w.kind == ENTITY_KIND_PILL, "kind %u", (unsigned)w.kind);
    UT_ASSERT_MSG(w.index == out.index,
                  "the event named index %u and the op reported %u",
                  (unsigned)w.index, (unsigned)out.index);
    UT_ASSERT_MSG(w.added == 1, "an add said added=%u", (unsigned)w.added);
    UT_ASSERT_MSG(w.recX == lx && w.recY == ly,
                  "the event carried %u,%u", (unsigned)w.recX,
                  (unsigned)w.recY);

    /* A base add, which appends. */
    UT_ASSERT(eaFindLand(sim, &lx, &ly));
    memset(&out, 0, sizeof(out));
    eaAddBaseOp(&op, lx, ly, NEUTRAL, 1, 2, 3);
    UT_ASSERT(eaApply(sim, &op, &out) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 3, "the base add published %d events", w.count);
    UT_ASSERT_MSG(w.kind == ENTITY_KIND_BASE, "kind %u", (unsigned)w.kind);
    UT_ASSERT_MSG(w.index == baseCount,
                  "the base event named index %u, expected %u",
                  (unsigned)w.index, (unsigned)baseCount);
    UT_ASSERT(w.added == 1);

    /* A base removal. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, 0);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 4, "the base removal published %d events",
                  w.count);
    UT_ASSERT(w.kind == ENTITY_KIND_BASE);
    UT_ASSERT(w.index == 0);
    UT_ASSERT(w.added == 0);

    /* A start removal, then the add back. */
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_START, 7);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 5, "the start removal published %d events",
                  w.count);
    UT_ASSERT(w.kind == ENTITY_KIND_START);
    UT_ASSERT(w.index == 7);
    UT_ASSERT(w.added == 0);

    memset(&out, 0, sizeof(out));
    eaAddStartOp(&op, sx, sy, 11);
    UT_ASSERT(eaApply(sim, &op, &out) == SCN_OP_OK);
    UT_ASSERT_MSG(w.count == 6, "the start add published %d events", w.count);
    UT_ASSERT(w.kind == ENTITY_KIND_START);
    UT_ASSERT_MSG(w.index == out.index && w.index == 7,
                  "the start add named index %u", (unsigned)w.index);
    UT_ASSERT(w.added == 1);
    UT_ASSERT_MSG(w.recX == sx && w.recY == sy,
                  "the start event carried %u,%u", (unsigned)w.recX,
                  (unsigned)w.recY);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ── out may be NULL ─────────────────────────────────────────────── */

/* A script that does not want the index back passes nothing, and the three
 * adds still land. */
int run_scenario_entity_add_out_null(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    BYTE lx = 0, ly = 0, sx = 0, sy = 0;
    BYTE baseCount;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    UT_ASSERT(eaFindLand(sim, &lx, &ly));
    UT_ASSERT(eaFindDeepSea(sim, &sx, &sy));
    baseCount = basesGetNumBases(&gs->bs);

    UT_ASSERT(pillsRemoveItem(&gs->pb, 1) == TRUE);
    eaAddPillOp(&op, lx, ly, NEUTRAL, 5, 70);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "a pillbox add with no out was refused");
    UT_ASSERT(pillsIsActive(&gs->pb, 1) == TRUE);

    UT_ASSERT(eaFindLand(sim, &lx, &ly));
    eaAddBaseOp(&op, lx, ly, NEUTRAL, 1, 1, 1);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "a base add with no out was refused");
    UT_ASSERT(basesIsActive(&gs->bs, (BYTE)(baseCount + 1)) == TRUE);

    UT_ASSERT(startsRemoveItem(&gs->ss, 1) == TRUE);
    eaAddStartOp(&op, sx, sy, 1);
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "a start add with no out was refused");
    UT_ASSERT(startsIsActive(&gs->ss, 1) == TRUE);

    serverSimDestroy(sim);
    return 0;
}

/* ── A removed item is not an item ───────────────────────────────── */

/* Every arm that takes a pill or base index answers SCN_OP_NO_SUCH_ITEM for
 * a slot a removal has emptied, not only for one past the count; the reads
 * that draw and report items answer as for a slot the map does not use; and
 * a map saved with a removal writes only what is on the map. */
int run_scenario_removed_item_is_no_item(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    GameSim *gs;
    ScenarioOp op;
    BYTE pills, bases;
    BYTE shells = 9, mines = 9, armour = 9;

    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    pills = pillsGetNumPills(&gs->pb);
    bases = basesGetNumBases(&gs->bs);
    UT_ASSERT(pills >= 2 && bases >= 2);

    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_PILL, 0);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_OK);
    eaRemoveOp(&op, SCN_OP_ENTITY_REMOVE_BASE, 0);
    UT_ASSERT(eaApply(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_OWNER;
    op.u.pillSetOwner.pill = 0;
    op.u.pillSetOwner.owner = 0;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "set-owner accepted a removed pillbox");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_ARMOUR;
    op.u.pillSetArmour.pill = 0;
    op.u.pillSetArmour.armour = 5;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "set-armour accepted a removed pillbox");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_SPEED;
    op.u.pillSetSpeed.pill = 0;
    op.u.pillSetSpeed.speed = 50;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "set-speed accepted a removed pillbox");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_MOVE;
    op.u.pillMove.pill = 0;
    UT_ASSERT_MSG(eaFindLand(sim, &op.u.pillMove.x, &op.u.pillMove.y),
                  "setup: the map has no free land square");
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "move accepted a removed pillbox");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_TANK_GIVE_PILL;
    op.u.tankGivePill.slot = 0;
    op.u.tankGivePill.pill = 0;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "give-pill handed out a removed pillbox");
    UT_ASSERT_MSG(pillsIsActive(&gs->pb, 1) == FALSE,
                  "a refused arm put the pillbox back on the map");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base = 0;
    op.u.baseSetOwner.owner = 0;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "base set-owner accepted a removed base");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_STOCK;
    op.u.baseSetStock.base = 0;
    op.u.baseSetStock.armour = 10;
    op.u.baseSetStock.shells = 10;
    op.u.baseSetStock.mines = 10;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM,
                  "base set-stock accepted a removed base");

    /* The live neighbours still take the same arms. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_PILL_SET_ARMOUR;
    op.u.pillSetArmour.pill = 1;
    op.u.pillSetArmour.armour = 5;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "set-armour refused the pillbox beside the removed one");
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base = 1;
    op.u.baseSetOwner.owner = NEUTRAL;
    UT_ASSERT_MSG(eaApply(sim, &op, NULL) == SCN_OP_OK,
                  "base set-owner refused the base beside the removed one");

    /* The reads that draw and report an item answer as for an empty slot. */
    UT_ASSERT_MSG(pillsGetAllianceNum(gs, &gs->pb, 1) == pillNeutral,
                  "the status panel still has a colour for a removed pillbox");
    UT_ASSERT_MSG(basesGetStatusNum(gs, 1) == baseNeutral,
                  "the status panel still has a colour for a removed base");
    basesGetStats(&gs->bs, 1, &shells, &mines, &armour);
    UT_ASSERT_MSG(shells == 0 && mines == 0 && armour == 0,
                  "a removed base still reports stock %u/%u/%u",
                  (unsigned)shells, (unsigned)mines, (unsigned)armour);
    UT_ASSERT_MSG(pillsGetNumActive(&gs->pb) == (BYTE)(pills - 1) &&
                  basesGetNumActive(&gs->bs) == (BYTE)(bases - 1),
                  "the live counts are %u and %u after one removal each",
                  (unsigned)pillsGetNumActive(&gs->pb),
                  (unsigned)basesGetNumActive(&gs->bs));

    /* A map saved now holds the items on the map, not the slots. The header
       is the eight-byte magic, the version, then the three counts. */
    {
        const char *path = "test_scenario_removed_item_save.map";
        FILE *fp;
        unsigned char hdr[12];
        UT_ASSERT_MSG(mapWrite((char *)path, &gs->mp, &gs->pb, &gs->bs, &gs->ss),
                      "mapWrite failed");
        fp = fopen(path, "rb");
        UT_ASSERT_MSG(fp != NULL, "the saved map could not be reopened");
        UT_ASSERT(fread(hdr, 1, sizeof(hdr), fp) == sizeof(hdr));
        fclose(fp);
        remove(path);
        UT_ASSERT_MSG(hdr[9] == (unsigned char)(pills - 1),
                      "the saved map counts %u pillboxes, wanted %u",
                      (unsigned)hdr[9], (unsigned)(pills - 1));
        UT_ASSERT_MSG(hdr[10] == (unsigned char)(bases - 1),
                      "the saved map counts %u bases, wanted %u",
                      (unsigned)hdr[10], (unsigned)(bases - 1));
        UT_ASSERT_MSG(hdr[11] == startsGetNumActive(&gs->ss),
                      "the saved map counts %u starts, wanted %u",
                      (unsigned)hdr[11], (unsigned)startsGetNumActive(&gs->ss));
    }

    serverSimDestroy(sim);
    return 0;
}

/*
 * Pure viewport square calculator (test_viewport_calc.c).
 *
 * viewportCalcSquarePure turns one map square into a tilenum.h sprite index
 * without touching the sim; viewportCalcSquare wraps it and keeps the two
 * side effects the main view still needs — the invisiwall terrain write-back
 * and the mineView store. This pins the two together: over the whole 256x256
 * map the pure tile and its mine flag match what the wrapper produces, no
 * square ever comes back as TANK_TRANSPARENT, and pill and base squares
 * resolve by alliance for whichever player is asking. It also covers the view
 * fill: a square a sight mask calls hidden draws the tile last seen there
 * rather than the one that is there, while the brain map keeps the real
 * terrain either way.
 */

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim_internal.h" /* struct ViewPort — vp.mineView */
#include "viewport.h"
#include "sight.h"    /* SIGHT_MASK_BYTES — the buffer a sight mask lives in */
#include "bolo_map.h" /* mapSetPos — plants a raw terrain byte */
#include "mines.h"    /* minesRemoveItem */
#include "bases.h"
#include "pillbox.h"
#include "tilenum.h"
#include "test_harness.h"

/* PILL_GOOD_15..PILL_GOOD_0 are contiguous; the evil tiles run
 * PILL_EVIL_14..PILL_EVIL_0 with PILL_EVIL_15 sitting on its own. */
static bool tileIsPillGood(BYTE tile) {
    return tile >= PILL_GOOD_15 && tile <= PILL_GOOD_0;
}

static bool tileIsPillEvil(BYTE tile) {
    return tile == PILL_EVIL_15 ||
           (tile >= PILL_EVIL_14 && tile <= PILL_EVIL_0);
}

int run_viewport_calc_square_pure(void) {
    ServerSim *sim = ut_make_running_sim("View");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");

    ViewPort vp;
    viewportInit(&vp);

    int x, y;

    /* Agreement over every square. The pure call has to come first — the
     * wrapper may normalise the square before it delegates. */
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            bool pureMine = FALSE;
            BYTE pureTile =
                viewportCalcSquarePure(gs, 0, (BYTE)x, (BYTE)y, &pureMine);
            BYTE wrapTile = viewportCalcSquare(&vp, gs, 0, (BYTE)x, (BYTE)y, 0, 0);
            bool wrapMine = vp.mineView->mineItem[0][0];

            UT_ASSERT_MSG(pureTile == wrapTile,
                          "square (%d,%d): pure tile %u, wrapper tile %u",
                          x, y, pureTile, wrapTile);
            UT_ASSERT_MSG(pureMine == wrapMine,
                          "square (%d,%d): pure mine %d, wrapper mineView %d",
                          x, y, (int)pureMine, (int)wrapMine);
            /* 255 is reserved as the overview's unseen sentinel, so the
             * calculator must never produce it as a real tile. */
            UT_ASSERT_MSG(pureTile != TANK_TRANSPARENT,
                          "square (%d,%d): pure tile is TANK_TRANSPARENT (%u)",
                          x, y, pureTile);
        }
    }

    /* Pill alliance — park pill 0 on a known square and read it back as its
     * owner and as a stranger. Player 3 was never added; playersIsAllie
     * handles that without indexing an unused slot. */
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "map has no pillboxes (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));

    const BYTE pillX = 100, pillY = 100;
    gs->pb->item[0].owner = 0;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].x = pillX;
    gs->pb->item[0].y = pillY;

    bool mineFlag = FALSE;
    BYTE pillOwnTile = viewportCalcSquarePure(gs, 0, pillX, pillY, &mineFlag);
    UT_ASSERT_MSG(tileIsPillGood(pillOwnTile),
                  "pill (%u,%u) owned by 0 seen by 0: tile %u is outside "
                  "PILL_GOOD_15..PILL_GOOD_0 (%u..%u)",
                  pillX, pillY, pillOwnTile, (BYTE)PILL_GOOD_15,
                  (BYTE)PILL_GOOD_0);

    BYTE pillEnemyTile = viewportCalcSquarePure(gs, 3, pillX, pillY, &mineFlag);
    UT_ASSERT_MSG(tileIsPillEvil(pillEnemyTile),
                  "pill (%u,%u) owned by 0 seen by 3: tile %u is not a "
                  "PILL_EVIL tile (%u or %u..%u)",
                  pillX, pillY, pillEnemyTile, (BYTE)PILL_EVIL_15,
                  (BYTE)PILL_EVIL_14, (BYTE)PILL_EVIL_0);

    /* Base alliance — same square read as owner, stranger and neutral. */
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) >= 1,
                  "map has no bases (%u) — test needs one",
                  basesGetNumBases(&gs->bs));

    BYTE baseX = gs->bs->item[0].x;
    BYTE baseY = gs->bs->item[0].y;
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, baseX, baseY) == FALSE,
                  "base 0 (%u,%u) also holds a pillbox — the pill branch "
                  "would answer instead of the base branch",
                  baseX, baseY);

    /* Above MIN_ARMOUR_CAPTURE so the base is not read as dead. */
    gs->bs->item[0].armour = BASE_FULL_ARMOUR;

    gs->bs->item[0].owner = 0;
    BYTE baseOwnTile = viewportCalcSquarePure(gs, 0, baseX, baseY, &mineFlag);
    UT_ASSERT_MSG(baseOwnTile == BASE_GOOD,
                  "base (%u,%u) owned by 0 seen by 0: tile %u, expected "
                  "BASE_GOOD (%u)",
                  baseX, baseY, baseOwnTile, (BYTE)BASE_GOOD);

    BYTE baseEnemyTile = viewportCalcSquarePure(gs, 3, baseX, baseY, &mineFlag);
    UT_ASSERT_MSG(baseEnemyTile == BASE_EVIL,
                  "base (%u,%u) owned by 0 seen by 3: tile %u, expected "
                  "BASE_EVIL (%u)",
                  baseX, baseY, baseEnemyTile, (BYTE)BASE_EVIL);

    gs->bs->item[0].owner = NEUTRAL;
    BYTE baseNeutralTile = viewportCalcSquarePure(gs, 0, baseX, baseY, &mineFlag);
    UT_ASSERT_MSG(baseNeutralTile == BASE_NEUTRAL,
                  "unowned base (%u,%u) seen by 0: tile %u, expected "
                  "BASE_NEUTRAL (%u)",
                  baseX, baseY, baseNeutralTile, (BYTE)BASE_NEUTRAL);

    /* Invisiwall normalisation — the branch the map as loaded never reaches,
     * since it holds nothing but terrain 0..15 and DEEP_SEA. A raw byte of
     * MINE_SWAMP + MINE_SUBTRACT normalises to MINE_SWAMP, which the mine
     * branch strips again down to SWAMP; SWAMP is not a case in the terminal
     * switch, so the tile is the terrain byte itself and no adjacency is
     * involved. Reading the raw byte instead of the normalised one would put
     * 18 outside MINE_START..MINE_END, skip the second subtraction and leave
     * MINE_SWAMP. */
    const BYTE plantRaw = (BYTE)(MINE_SWAMP + MINE_SUBTRACT);
    const BYTE plantX = 150, plantY = 200;

    UT_ASSERT_MSG(pillsExistPos(&gs->pb, plantX, plantY) == FALSE,
                  "plant square (%u,%u) holds a pillbox — the pill branch "
                  "would answer instead of the terrain branch",
                  plantX, plantY);
    UT_ASSERT_MSG(basesExistPos(&gs->bs, plantX, plantY) == FALSE,
                  "plant square (%u,%u) holds a base — the base branch "
                  "would answer instead of the terrain branch",
                  plantX, plantY);

    /* Modelled: the calculator normalises locally and answers mapIsMine and
     * minesExistPos against that value and a cleared overlay cell. */
    mapSetPos(gs, &gs->mp, plantX, plantY, plantRaw, TRUE, TRUE);
    bool mineModelled = FALSE;
    BYTE tileModelled =
        viewportCalcSquarePure(gs, 0, plantX, plantY, &mineModelled);

    UT_ASSERT_MSG(tileModelled == SWAMP,
                  "planted %u at (%u,%u): modelled tile %u, expected SWAMP (%u)",
                  plantRaw, plantX, plantY, tileModelled, (BYTE)SWAMP);

    /* Real: perform the repair by hand, exactly as the wrapper does, then
     * recompute against the written-back byte and the cleared cell. */
    minesRemoveItem(&gs->mns, plantX, plantY);
    mapSetPos(gs, &gs->mp, plantX, plantY, (BYTE)(plantRaw - MINE_SUBTRACT),
              TRUE, TRUE);
    bool mineReal = FALSE;
    BYTE tileReal = viewportCalcSquarePure(gs, 0, plantX, plantY, &mineReal);

    UT_ASSERT_MSG(tileModelled == tileReal,
                  "planted %u at (%u,%u): modelled tile %u, repaired tile %u",
                  plantRaw, plantX, plantY, tileModelled, tileReal);
    UT_ASSERT_MSG(mineModelled == mineReal,
                  "planted %u at (%u,%u): modelled mine %d, repaired mine %d",
                  plantRaw, plantX, plantY, (int)mineModelled, (int)mineReal);

    /* Border square. mapGetPos returns DEEP_SEA for anything on or outside
     * MAP_MINE_EDGE_*, the same bounds the two modelled helpers test, so no
     * terrain byte stored out here is ever readable and both helpers answer on
     * position alone: a mine is always reported and the tile is always the
     * solid deep sea one, with every neighbour reading DEEP_SEA too. */
    const BYTE borderX = 10, borderY = 150;
    bool borderMine = FALSE;
    BYTE borderTile =
        viewportCalcSquarePure(gs, 0, borderX, borderY, &borderMine);

    UT_ASSERT_MSG(borderMine == TRUE,
                  "border square (%u,%u): mine %d, expected TRUE whatever the "
                  "terrain says",
                  borderX, borderY, (int)borderMine);
    UT_ASSERT_MSG(borderTile == DEEP_SEA_SOLID,
                  "border square (%u,%u): tile %u, expected DEEP_SEA_SOLID (%u)",
                  borderX, borderY, borderTile, (BYTE)DEEP_SEA_SOLID);

    /* Edge squares, re-checked against the mutated sim: the neighbour reads
     * wrap through BYTE arithmetic here and the border makes mapIsMine and
     * minesExistPos answer TRUE whatever the terrain says. */
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        bool pureMine = FALSE;
        BYTE pureTile = viewportCalcSquarePure(gs, 0, 0, (BYTE)y, &pureMine);
        BYTE wrapTile = viewportCalcSquare(&vp, gs, 0, 0, (BYTE)y, 0, 0);
        bool wrapMine = vp.mineView->mineItem[0][0];

        UT_ASSERT_MSG(pureTile == wrapTile,
                      "left edge (0,%d): pure tile %u, wrapper tile %u",
                      y, pureTile, wrapTile);
        UT_ASSERT_MSG(pureMine == wrapMine,
                      "left edge (0,%d): pure mine %d, wrapper mineView %d",
                      y, (int)pureMine, (int)wrapMine);
    }

    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        bool pureMine = FALSE;
        BYTE pureTile = viewportCalcSquarePure(gs, 0, (BYTE)x, 255, &pureMine);
        BYTE wrapTile = viewportCalcSquare(&vp, gs, 0, (BYTE)x, 255, 0, 0);
        bool wrapMine = vp.mineView->mineItem[0][0];

        UT_ASSERT_MSG(pureTile == wrapTile,
                      "bottom edge (%d,255): pure tile %u, wrapper tile %u",
                      x, pureTile, wrapTile);
        UT_ASSERT_MSG(pureMine == wrapMine,
                      "bottom edge (%d,255): pure mine %d, wrapper mineView %d",
                      x, (int)pureMine, (int)wrapMine);
    }

    /* Line of sight: a square the mask calls hidden draws the tile the player
     * last saw there, not the tile that is there now, and says so in the
     * hidden view. A seen square draws what is there. The mask is written by
     * hand — which squares sight reaches is test_sight's question, and this is
     * only about what the view does with the answer. */
    {
        const BYTE viewLeft = 60, viewTop = 60;
        const BYTE hiddenSX = 5, hiddenSY = 5;   /* Back-buffer square hidden */
        const BYTE seenSX = 6, seenSY = 5;       /* One beside it, seen */
        const BYTE hiddenMX = (BYTE)(viewLeft + hiddenSX);
        const BYTE hiddenMY = (BYTE)(viewTop + hiddenSY);
        const BYTE seenMX = (BYTE)(viewLeft + seenSX);
        const BYTE seenMY = (BYTE)(viewTop + seenSY);
        const BYTE brainSentinel = 0xEE;
        BYTE (*brainMap)[MAP_ARRAY_SIZE] =
            (BYTE (*)[MAP_ARRAY_SIZE])malloc(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
        OverviewMap *om = (OverviewMap *)malloc(sizeof(OverviewMap));
        BYTE vis[SIGHT_MASK_BYTES];
        ViewSight sight;
        bool ignoreMine = FALSE;
        BYTE realHidden, realSeen, remembered, brainMasked, brainPlain;
        int stride;

        UT_ASSERT_MSG(brainMap != NULL && om != NULL,
                      "out of memory setting up the sight case");

        realHidden =
            viewportCalcSquarePure(gs, 0, hiddenMX, hiddenMY, &ignoreMine);
        realSeen = viewportCalcSquarePure(gs, 0, seenMX, seenMY, &ignoreMine);

        /* Something the square demonstrably does not show now, so the tile the
         * view ends up with says which of the two sources it came from. */
        remembered =
            (BYTE)(realHidden == DEEP_SEA_SOLID ? SWAMP : DEEP_SEA_SOLID);
        UT_ASSERT_MSG(remembered != realHidden,
                      "square (%u,%u): the remembered tile and the real one "
                      "are both %u, so the case cannot tell them apart",
                      hiddenMX, hiddenMY, realHidden);

        memset(om, 0, sizeof(*om));
        om->tile[hiddenMX][hiddenMY] = remembered;

        memset(&sight, 0, sizeof(sight));
        sight.block.left = (int)viewLeft;
        sight.block.top = (int)viewTop;
        sight.block.right = sight.block.left + MAIN_BACK_BUFFER_SIZE_X - 1;
        sight.block.bottom = sight.block.top + MAIN_BACK_BUFFER_SIZE_Y - 1;
        stride = sight.block.right - sight.block.left + 1;
        memset(vis, 1, sizeof(vis));
        vis[hiddenSY * stride + hiddenSX] = 0;
        sight.vis = vis;
        sight.memory = om;

        vp.xOffset = viewLeft;
        vp.yOffset = viewTop;

        memset(brainMap, brainSentinel, MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
        viewportUpdateView(&vp, gs, 0, brainMap, (updateType)0, &sight);
        brainMasked = brainMap[hiddenMY][hiddenMX];

        UT_ASSERT_MSG(vp.view->screenItem[hiddenSX][hiddenSY] == remembered,
                      "hidden square (%u,%u): drew tile %u, expected the "
                      "remembered %u",
                      hiddenMX, hiddenMY,
                      vp.view->screenItem[hiddenSX][hiddenSY], remembered);
        UT_ASSERT_MSG(vp.hiddenView->hiddenItem[hiddenSX][hiddenSY] == TRUE,
                      "hidden square (%u,%u): hidden view says %d, expected 1",
                      hiddenMX, hiddenMY,
                      (int)vp.hiddenView->hiddenItem[hiddenSX][hiddenSY]);
        UT_ASSERT_MSG(vp.view->screenItem[seenSX][seenSY] == realSeen,
                      "seen square (%u,%u): drew tile %u, expected the real %u",
                      seenMX, seenMY, vp.view->screenItem[seenSX][seenSY],
                      realSeen);
        UT_ASSERT_MSG(vp.hiddenView->hiddenItem[seenSX][seenSY] == FALSE,
                      "seen square (%u,%u): hidden view says %d, expected 0",
                      seenMX, seenMY,
                      (int)vp.hiddenView->hiddenItem[seenSX][seenSY]);
        UT_ASSERT_MSG(brainMasked != brainSentinel,
                      "hidden square (%u,%u): the brain map was never written",
                      hiddenMX, hiddenMY);

        /* No mask at all: every square back to what is there, nothing hidden. */
        memset(brainMap, brainSentinel, MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
        viewportUpdateView(&vp, gs, 0, brainMap, (updateType)0, NULL);
        brainPlain = brainMap[hiddenMY][hiddenMX];

        UT_ASSERT_MSG(vp.view->screenItem[hiddenSX][hiddenSY] == realHidden,
                      "no mask, square (%u,%u): drew tile %u, expected the "
                      "real %u",
                      hiddenMX, hiddenMY,
                      vp.view->screenItem[hiddenSX][hiddenSY], realHidden);
        UT_ASSERT_MSG(vp.hiddenView->hiddenItem[hiddenSX][hiddenSY] == FALSE,
                      "no mask, square (%u,%u): hidden view says %d, "
                      "expected 0",
                      hiddenMX, hiddenMY,
                      (int)vp.hiddenView->hiddenItem[hiddenSX][hiddenSY]);
        /* The brain map is fed the real terrain either way: bots read it, and
         * substituting for them is not the view's business. */
        UT_ASSERT_MSG(brainMasked == brainPlain,
                      "square (%u,%u): the brain map holds %u under the mask "
                      "and %u without it",
                      hiddenMX, hiddenMY, brainMasked, brainPlain);

        free(om);
        free(brainMap);
    }

    viewportDestroy(&vp);
    serverSimDestroy(sim);
    return 0;
}

/* A pill this client watched get carried off draws nothing at the square it
 * was taken from. viewportCalcSquarePure is the only thing that writes the
 * overview's tile memory — overviewStampRect goes through it — so this is
 * where the game view and the overview agree to show the terrain underneath
 * instead. A pill whose square is merely remembered still draws as a pill. */
int run_viewport_calc_pill_square_moved(void) {
    ServerSim *sim = ut_make_running_sim("Move");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= 1,
                  "map has no pillboxes (%u) — test needs one",
                  pillsGetNumPills(&gs->pb));

    /* A square holding neither a pill nor a base, so the tile it answers with
     * is plain terrain and only the pill branch can change it. */
    BYTE sqX = 0, sqY = 0;
    bool found = FALSE;
    int x, y;

    for (x = 100; x < 160 && found == FALSE; x++) {
        for (y = 100; y < 160 && found == FALSE; y++) {
            if (pillsViewExistPos(&gs->pb, (BYTE)x, (BYTE)y) == FALSE &&
                basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) == FALSE) {
                sqX = (BYTE)x;
                sqY = (BYTE)y;
                found = TRUE;
            }
        }
    }
    UT_ASSERT_MSG(found == TRUE,
                  "no square clear of pills and bases in the middle of the map");

    bool bareMine = FALSE;
    BYTE bareTile = viewportCalcSquarePure(gs, 0, sqX, sqY, &bareMine);

    /* Park pill 0 on it with its square only remembered — the pill has not
     * been touched, we have just lost sight of it. */
    gs->pb->item[0].owner = 0;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].x = sqX;
    gs->pb->item[0].y = sqY;
    pillsSetPosState(&gs->pb, 0, PILL_SQUARE_REMEMBERED);

    bool rememberedMine = FALSE;
    BYTE rememberedTile =
        viewportCalcSquarePure(gs, 0, sqX, sqY, &rememberedMine);
    UT_ASSERT_MSG(tileIsPillGood(rememberedTile),
                  "remembered pill at (%u,%u): tile %u is outside "
                  "PILL_GOOD_15..PILL_GOOD_0 (%u..%u)",
                  sqX, sqY, rememberedTile, (BYTE)PILL_GOOD_15,
                  (BYTE)PILL_GOOD_0);

    /* Watched it go into a tank and come out somewhere we cannot see: the
     * square reads exactly as it did before the pill was ever on it. */
    pillsSetPosState(&gs->pb, 0, PILL_SQUARE_MOVED);
    bool movedMine = FALSE;
    BYTE movedTile = viewportCalcSquarePure(gs, 0, sqX, sqY, &movedMine);
    UT_ASSERT_MSG(movedTile == bareTile,
                  "moved pill at (%u,%u): tile %u, expected the terrain "
                  "underneath (%u)",
                  sqX, sqY, movedTile, bareTile);
    UT_ASSERT_MSG(movedMine == bareMine,
                  "moved pill at (%u,%u): mine %d, expected %d",
                  sqX, sqY, (int)movedMine, (int)bareMine);

    /* The true square arriving draws it again. */
    pillsSetPosState(&gs->pb, 0, PILL_SQUARE_CONFIRMED);
    bool confirmedMine = FALSE;
    BYTE confirmedTile =
        viewportCalcSquarePure(gs, 0, sqX, sqY, &confirmedMine);
    UT_ASSERT_MSG(confirmedTile == rememberedTile,
                  "confirmed pill at (%u,%u): tile %u, expected the pill tile "
                  "(%u)",
                  sqX, sqY, confirmedTile, rememberedTile);

    serverSimDestroy(sim);
    return 0;
}

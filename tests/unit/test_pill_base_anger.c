/* Which allied pills a shot base heats: the WinBolo square, inclusive, by
 * default, and the Mac Bolo circle, exclusive, when the shape rule asks. */
#include "global.h"
#include "pillbox.h"
#include "game_sim.h"
#include "client_sim.h"
#include "test_harness.h"

int run_pill_base_anger_radius(void) {
    static const struct {
        int shape;
        int range;
        int dx;
        int dy;
        bool angry;
    } cases[] = {
        /* The classic square takes the range on each axis, edge included. */
        { PILL_BASE_HIT_SQUARE, 9, 0, 0, TRUE },
        { PILL_BASE_HIT_SQUARE, 9, 9, 0, TRUE },
        { PILL_BASE_HIT_SQUARE, 9, 9, 9, TRUE },
        { PILL_BASE_HIT_SQUARE, 9, 7, 0, TRUE },
        { PILL_BASE_HIT_SQUARE, 9, 5, 5, TRUE },
        { PILL_BASE_HIT_SQUARE, 9, 10, 0, FALSE },
        { PILL_BASE_HIT_SQUARE, 9, 10, 9, FALSE },
        { PILL_BASE_HIT_SQUARE, 4, 4, 4, TRUE },
        { PILL_BASE_HIT_SQUARE, 4, 5, 0, FALSE },
        { PILL_BASE_HIT_SQUARE, 0, 0, 0, TRUE },
        { PILL_BASE_HIT_SQUARE, 0, 1, 0, FALSE },
        /* Mac Bolo heats allied pills strictly inside a seven-tile circle. */
        { PILL_BASE_HIT_CIRCLE, 7, 1, 0, TRUE },
        { PILL_BASE_HIT_CIRCLE, 7, 6, 0, TRUE },
        { PILL_BASE_HIT_CIRCLE, 7, 6, 3, TRUE },
        { PILL_BASE_HIT_CIRCLE, 7, 4, 5, TRUE },
        { PILL_BASE_HIT_CIRCLE, 7, 7, 0, FALSE },
        { PILL_BASE_HIT_CIRCLE, 7, 5, 5, FALSE },
        { PILL_BASE_HIT_CIRCLE, 7, 6, 4, FALSE },
        { PILL_BASE_HIT_CIRCLE, 7, 9, 9, FALSE },
        /* Custom rules must move the circle and keep its edge exclusive. */
        { PILL_BASE_HIT_CIRCLE, 5, 2, 4, TRUE },
        { PILL_BASE_HIT_CIRCLE, 5, 3, 4, FALSE },
        { PILL_BASE_HIT_CIRCLE, 5, 5, 0, FALSE },
        { PILL_BASE_HIT_CIRCLE, 10, 7, 7, TRUE },
        { PILL_BASE_HIT_CIRCLE, 10, 6, 8, FALSE },
        { PILL_BASE_HIT_CIRCLE, 10, 10, 0, FALSE },
        { PILL_BASE_HIT_CIRCLE, 0, 0, 0, FALSE }
    };
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    size_t i;
    int swap, sx, sy;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs->rules.pill_base_defend_range == 9);
    UT_ASSERT(gs->rules.pill_base_defend_shape == PILL_BASE_HIT_SQUARE);
    pillsSetNumPills(&gs->pb, 1);
    (*gs->pb).active[0] = TRUE;
    (*gs->pb).item[0].owner = 0;
    (*gs->pb).item[0].armour = (BYTE)gs->rules.pill_max_armour;

    /* Reflect and swap each offset to cover every quadrant and both axes. */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        gs->rules.pill_base_defend_shape = cases[i].shape;
        gs->rules.pill_base_defend_range = cases[i].range;
        for (swap = 0; swap < 2; swap++) {
            for (sx = -1; sx <= 1; sx += 2) {
                for (sy = -1; sy <= 1; sy += 2) {
                    int dx = sx * (swap ? cases[i].dy : cases[i].dx);
                    int dy = sy * (swap ? cases[i].dx : cases[i].dy);
                    (*gs->pb).item[0].x = (BYTE)(128 + dx);
                    (*gs->pb).item[0].y = (BYTE)(128 + dy);
                    (*gs->pb).item[0].speed = 100;
                    (*gs->pb).item[0].coolDown = 0;

                    pillsBaseHit(gs, &gs->pb, 128, 128, 0);

                    UT_ASSERT_MSG((*gs->pb).item[0].speed == (cases[i].angry ? 50 : 100),
                                  "shape %d range %d offset (%d,%d): speed=%u, angry=%d",
                                  cases[i].shape, cases[i].range, dx, dy,
                                  (*gs->pb).item[0].speed, cases[i].angry);
                    UT_ASSERT_MSG((*gs->pb).item[0].coolDown ==
                                  (cases[i].angry ? gs->rules.pill_cooldown_ticks : 0),
                                  "shape %d range %d offset (%d,%d): cooldown=%u",
                                  cases[i].shape, cases[i].range, dx, dy,
                                  (*gs->pb).item[0].coolDown);
                }
            }
        }
    }

    clientSimDestroy(cs);
    return 0;
}

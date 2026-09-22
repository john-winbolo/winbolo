/* Mac Bolo heats allied pills strictly inside a seven-tile circle. */
#include "global.h"
#include "pillbox.h"
#include "game_sim.h"
#include "client_sim.h"
#include "test_harness.h"

int run_pill_base_anger_radius(void) {
    static const struct {
        int range;
        int dx;
        int dy;
        bool angry;
    } cases[] = {
        { 7, 1, 0, TRUE },
        { 7, 6, 0, TRUE },
        { 7, 6, 3, TRUE },
        { 7, 4, 5, TRUE },
        { 7, 7, 0, FALSE },
        { 7, 5, 5, FALSE },
        { 7, 6, 4, FALSE },
        { 7, 9, 9, FALSE },
        /* Custom rules must move the circle and keep its edge exclusive. */
        { 5, 2, 4, TRUE },
        { 5, 3, 4, FALSE },
        { 5, 5, 0, FALSE },
        { 10, 7, 7, TRUE },
        { 10, 6, 8, FALSE },
        { 10, 10, 0, FALSE },
        { 0, 0, 0, FALSE }
    };
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    size_t i;
    int swap, sx, sy;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs->rules.pill_base_defend_range == 7);
    pillsSetNumPills(&gs->pb, 1);
    (*gs->pb).active[0] = TRUE;
    (*gs->pb).item[0].owner = 0;
    (*gs->pb).item[0].armour = (BYTE)gs->rules.pill_max_armour;

    /* Reflect and swap each offset to cover every quadrant and both axes. */
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
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
                                  "offset (%d,%d): speed=%u, angry=%d", dx, dy,
                                  (*gs->pb).item[0].speed, cases[i].angry);
                    UT_ASSERT_MSG((*gs->pb).item[0].coolDown ==
                                  (cases[i].angry ? gs->rules.pill_cooldown_ticks : 0),
                                  "offset (%d,%d): cooldown=%u", dx, dy,
                                  (*gs->pb).item[0].coolDown);
                }
            }
        }
    }

    clientSimDestroy(cs);
    return 0;
}

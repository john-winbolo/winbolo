/*
 * screenCalcRiver — the map square -> river tile lookup.
 *
 * The function is a 16-branch if/else chain over the four orthogonal
 * neighbours. Its contract is that a ROAD square counts as the river
 * continuing: a road laid across water is a bridge, and the river tiles
 * around it must not close off their banks against it. Every test in
 * the chain honoured that except the four RIVER_END1..4 arms, which
 * asked for `== RIVER` and so missed a road.
 *
 * The visible symptom: a river cross with single-tile arms and a road
 * on the centre square. Each arm has one road neighbour and three
 * grass, fails all four end tests, and falls through to a corner
 * (L-shaped) piece instead of the end U piece.
 *
 * Rather than pin all 81 outputs — a change-detector that locks in the
 * table and explains nothing — this asserts the two properties a reader
 * cannot check by eye:
 *
 *   1. The four orthogonal neighbours are read only as "wet or bridged".
 *      RIVER, ROAD, DEEP_SEA and BOAT are interchangeable there. This is
 *      the contract the bug violated; it fails on the pre-fix chain.
 *
 *   2. lv_screenCalcRiver (src/logviewer/screencalc.c) never disagrees
 *      with screenCalcRiver (src/bolo/screencalc.c). The two are
 *      hand-maintained character-for-character copies, and nothing else
 *      catches a fix applied to one and not the other.
 *
 * Both functions are already linked into this binary — screencalc.c via
 * bolo_static, src/logviewer/screencalc.c compiled directly — so this
 * needs no new build plumbing. The log-viewer prototype is declared here
 * instead of including lv_screencalc.h, which would pull lv_tilenum.h's
 * redefinitions of every tile number in on top of tilenum.h's.
 */
#include "screencalc.h"   /* screenCalcRiver, plus global.h + tilenum.h */

#include "test_harness.h"

BYTE lv_screenCalcRiver(BYTE aboveLeft, BYTE above, BYTE aboveRight,
                        BYTE left, BYTE right,
                        BYTE belowLeft, BYTE below, BYTE belowRight);

/* Every terrain value the function distinguishes: the four it treats as
 * wet-or-bridged, plus one dry square to stand for all the rest (the
 * chain only ever asks "is this RIVER or ROAD", so GRASS and BUILDING
 * take the same branches). */
static const BYTE kWet[] = { RIVER, ROAD, DEEP_SEA, BOAT };
#define WET_COUNT ((int)(sizeof(kWet) / sizeof(kWet[0])))

static const BYTE kTerrain[] = { GRASS, RIVER, ROAD, DEEP_SEA, BOAT };
#define TERRAIN_COUNT ((int)(sizeof(kTerrain) / sizeof(kTerrain[0])))

static int isWet(BYTE t) {
    for (int i = 0; i < WET_COUNT; i++) {
        if (kWet[i] == t) {
            return 1;
        }
    }
    return 0;
}

/* The eight neighbours in the argument order screenCalcRiver takes them.
 * Indices 1, 3, 4 and 6 are above / left / right / below — the four the
 * tile choice actually turns on. */
enum { N_ABOVE = 1, N_LEFT = 3, N_RIGHT = 4, N_BELOW = 6 };
static const int kOrthogonal[] = { N_ABOVE, N_LEFT, N_RIGHT, N_BELOW };

static BYTE callRiver(const BYTE n[8]) {
    return screenCalcRiver(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
}

/*
 * Property 1 — a road neighbour is a river neighbour.
 *
 * Sweeps all 5^8 neighbourhoods and, in each, substitutes every other
 * wet value into each wet orthogonal neighbour one at a time. The tile
 * must not move. On the pre-fix chain this trips on the four
 * neighbourhoods whose only wet square is a road.
 */
int run_screencalc_river_road_counts_as_water(void) {
    BYTE n[8];

    for (int i0 = 0; i0 < TERRAIN_COUNT; i0++) {
      for (int i1 = 0; i1 < TERRAIN_COUNT; i1++) {
        for (int i2 = 0; i2 < TERRAIN_COUNT; i2++) {
          for (int i3 = 0; i3 < TERRAIN_COUNT; i3++) {
            for (int i4 = 0; i4 < TERRAIN_COUNT; i4++) {
              for (int i5 = 0; i5 < TERRAIN_COUNT; i5++) {
                for (int i6 = 0; i6 < TERRAIN_COUNT; i6++) {
                  for (int i7 = 0; i7 < TERRAIN_COUNT; i7++) {
                    n[0] = kTerrain[i0]; n[1] = kTerrain[i1];
                    n[2] = kTerrain[i2]; n[3] = kTerrain[i3];
                    n[4] = kTerrain[i4]; n[5] = kTerrain[i5];
                    n[6] = kTerrain[i6]; n[7] = kTerrain[i7];

                    const BYTE expected = callRiver(n);

                    for (int p = 0; p < (int)(sizeof(kOrthogonal) /
                                             sizeof(kOrthogonal[0])); p++) {
                        const int  idx  = kOrthogonal[p];
                        const BYTE held = n[idx];
                        if (!isWet(held)) {
                            continue;
                        }
                        for (int w = 0; w < WET_COUNT; w++) {
                            if (kWet[w] == held) {
                                continue;
                            }
                            n[idx] = kWet[w];
                            const BYTE got = callRiver(n);
                            n[idx] = held;
                            UT_ASSERT_MSG(
                                got == expected,
                                "neighbour %d: %u -> %u changed the tile "
                                "from %u to %u (above=%u below=%u left=%u "
                                "right=%u) — RIVER and ROAD must be "
                                "interchangeable here",
                                idx, held, kWet[w], expected, got,
                                n[N_ABOVE], n[N_BELOW], n[N_LEFT], n[N_RIGHT]);
                        }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    return 0;
}

/*
 * The reported case, spelled out: a plus of river with a road on the
 * centre square. Each arm must draw the end U piece that points back at
 * the bridge, exactly as it would if the centre were still river.
 *
 *     . R .          R = river   O = road   . = grass
 *     R O R
 *     . R .
 */
int run_screencalc_river_arms_of_road_centred_cross(void) {
    struct {
        const char *what;
        BYTE        above, below, left, right;
        BYTE        expect;
    } cases[] = {
        /* Arm above the bridge: its river continues downward into it. */
        { "top arm",    GRASS, ROAD,  GRASS, GRASS, RIVER_END3 },
        /* Arm below the bridge. */
        { "bottom arm", ROAD,  GRASS, GRASS, GRASS, RIVER_END4 },
        /* Arm left of the bridge. */
        { "left arm",   GRASS, GRASS, GRASS, ROAD,  RIVER_END1 },
        /* Arm right of the bridge. */
        { "right arm",  GRASS, GRASS, ROAD,  GRASS, RIVER_END2 },
    };

    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        const BYTE got = screenCalcRiver(GRASS, cases[i].above, GRASS,
                                         cases[i].left, cases[i].right,
                                         GRASS, cases[i].below, GRASS);
        UT_ASSERT_MSG(got == cases[i].expect,
                      "%s of the road-centred cross drew tile %u, expected "
                      "the end piece %u",
                      cases[i].what, got, cases[i].expect);

        /* Control: swap the bridge back for river and nothing moves. */
        BYTE above = cases[i].above == ROAD ? RIVER : cases[i].above;
        BYTE below = cases[i].below == ROAD ? RIVER : cases[i].below;
        BYTE left  = cases[i].left  == ROAD ? RIVER : cases[i].left;
        BYTE right = cases[i].right == ROAD ? RIVER : cases[i].right;
        const BYTE plain = screenCalcRiver(GRASS, above, GRASS, left, right,
                                           GRASS, below, GRASS);
        UT_ASSERT_MSG(plain == cases[i].expect,
                      "%s with a river centre drew tile %u, expected %u",
                      cases[i].what, plain, cases[i].expect);
    }

    return 0;
}

/*
 * Property 2 — the log viewer's copy has not drifted.
 *
 * lv_screenCalcRiver is a hand-kept duplicate of screenCalcRiver. Sweep
 * every neighbourhood and require the two to agree everywhere, so a fix
 * (or a regression) landing in one file and not the other is caught here
 * rather than by someone noticing the replay renders differently from
 * the game.
 */
int run_screencalc_river_copies_agree(void) {
    for (int i0 = 0; i0 < TERRAIN_COUNT; i0++) {
      for (int i1 = 0; i1 < TERRAIN_COUNT; i1++) {
        for (int i2 = 0; i2 < TERRAIN_COUNT; i2++) {
          for (int i3 = 0; i3 < TERRAIN_COUNT; i3++) {
            for (int i4 = 0; i4 < TERRAIN_COUNT; i4++) {
              for (int i5 = 0; i5 < TERRAIN_COUNT; i5++) {
                for (int i6 = 0; i6 < TERRAIN_COUNT; i6++) {
                  for (int i7 = 0; i7 < TERRAIN_COUNT; i7++) {
                    const BYTE a = kTerrain[i0], b = kTerrain[i1];
                    const BYTE c = kTerrain[i2], d = kTerrain[i3];
                    const BYTE e = kTerrain[i4], f = kTerrain[i5];
                    const BYTE g = kTerrain[i6], h = kTerrain[i7];

                    const BYTE game   = screenCalcRiver(a, b, c, d, e, f, g, h);
                    const BYTE viewer = lv_screenCalcRiver(a, b, c, d, e, f,
                                                           g, h);
                    UT_ASSERT_MSG(game == viewer,
                                  "src/bolo and src/logviewer disagree at "
                                  "(above=%u below=%u left=%u right=%u): "
                                  "%u vs %u — the two copies of "
                                  "screenCalcRiver have drifted apart",
                                  b, g, d, e, game, viewer);
                  }
                }
              }
            }
          }
        }
      }
    }

    return 0;
}

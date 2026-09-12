/*
 * Overview regions under the server's view policies
 * (test_overview_view_policy.c).
 *
 * overviewMapBuildRegions turns what the player is allowed to see into the set
 * of map squares the overview treats as live. Each of pillboxes, bases and
 * allied tanks is on its own policy, and these cases pin what each one
 * produces: always gives every item of that kind the player could watch a
 * block, key gives one block on whatever the player is watching this moment
 * and closes the block round the player's own tank while it lasts, decay gives
 * a block to the items whose proximity clock is still running, and
 * off gives none. The blocks are the ones the matching view reaches — a base
 * takes the pill's 15x15 and an ally the tank's 29x29 — and they come out in a
 * fixed order the farewell stamp in overviewMapUpdate depends on: the tank,
 * then pills, bases and allied tanks each in ascending index.
 *
 * The first case is the one that has to keep holding: with the rules a server
 * ships with, and nobody allied, what comes out is exactly what the overview
 * has always drawn.
 *
 * Every case drives ut_make_running_sim and pokes the GameSim directly (the
 * unittests profile permits T2-internal access), replacing everything the map
 * shipped with so only what is set here can produce a region. The clocks are
 * written by hand rather than driven, so a whole decay window is walked
 * without a single tick of play. test_overview_map.c is where the client's own
 * clocks, and the view exit that reads them, are driven for real.
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_command.h" /* VIEW_KIND_* */
#include "view_policy.h"
#include "overview_map.h"
#include "allience.h"
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "test_harness.h"

/* Fails the calling test unless the rect is exactly these edges. */
#define ASSERT_RECT(r, l, t, rt, b)                                           \
    UT_ASSERT_MSG((r).left == (l) && (r).top == (t) && (r).right == (rt) &&    \
                      (r).bottom == (b),                                      \
                  "rect {%d,%d,%d,%d}, expected {%d,%d,%d,%d}",                \
                  (r).left, (r).top, (r).right, (r).bottom, (l), (t), (rt), (b))

/* Fails the calling test unless the rect is that block at that brightness. */
#define ASSERT_BLOCK(r, cx, cy, half, a, what)                                \
    do {                                                                      \
        ASSERT_RECT((r), (cx) - (half), (cy) - (half), (cx) + (half),          \
                    (cy) + (half));                                            \
        UT_ASSERT_MSG((r).alpha == (a), "%s has alpha %u, expected %u", (what), \
                      (unsigned)(r).alpha, (unsigned)(a));                     \
    } while (0)

/* The client's decay clocks run on its display tick, which is the game tick. */
#define VP_TICKS_SEC GAME_NUMGAMETICKS_SEC

/* Where the tank stands for every build below, well clear of every edge and of
 * everything else the world holds. */
#define VP_TANK_MX 100
#define VP_TANK_MY 100

/* One pillbox of the world below: index, owner and where it stands. */
static void vpPlacePill(GameSim *gs, BYTE idx, BYTE owner, BYTE mx, BYTE my) {
    gs->pb->item[idx].owner  = owner;
    gs->pb->item[idx].armour = PILLBOX_15;
    gs->pb->item[idx].inTank = FALSE;
    gs->pb->item[idx].x      = mx;
    gs->pb->item[idx].y      = my;
}

static void vpPlaceBase(GameSim *gs, BYTE idx, BYTE owner, BYTE mx, BYTE my) {
    gs->bs->item[idx].owner  = owner;
    gs->bs->item[idx].armour = BASE_FULL_ARMOUR;
    gs->bs->item[idx].x      = mx;
    gs->bs->item[idx].y      = my;
}

/* The square the client last saw a player's tank on — what the ally view
 * centres on, and the only position a client has for a remote tank. */
static void vpPlaceTank(GameSim *gs, BYTE slot, BYTE mx, BYTE my) {
    (*gs->plyrs).item[slot].mapX = mx;
    (*gs->plyrs).item[slot].mapY = my;
}

/* The world every case runs on. Slot 0 is us, slots 1 and 3 are allies and
 * slot 2 is hostile; there are two pillboxes we can view through and one we
 * cannot, two allied bases and one enemy one, and every tank is parked
 * somewhere of its own. Returns NULL if the sim could not be brought up. */
static ServerSim *vpMakeWorld(GameSim **outGs) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;

    if (sim == NULL) {
        return NULL;
    }
    serverSimAddPlayer(sim, 1, "Allie", false);
    serverSimAddPlayer(sim, 2, "Enemy", false);
    serverSimAddPlayer(sim, 3, "Allie2", false);

    gs = serverSimGetGameSim(sim);
    if (gs == NULL) {
        serverSimDestroy(sim);
        return NULL;
    }
    gs->viewPlayer = 0;
    allienceAdd(&(*gs->plyrs).item[0].allie, 1);
    allienceAdd(&(*gs->plyrs).item[1].allie, 0);
    allienceAdd(&(*gs->plyrs).item[0].allie, 3);
    allienceAdd(&(*gs->plyrs).item[3].allie, 0);

    pillsSetNumPills(&gs->pb, 3);
    vpPlacePill(gs, 0, 0, 200, 50);
    vpPlacePill(gs, 1, 1, 210, 60);
    vpPlacePill(gs, 2, 2, 220, 70);

    basesSetNumBases(&gs->bs, 3);
    vpPlaceBase(gs, 0, 0, 40, 40);
    vpPlaceBase(gs, 1, 2, 60, 40);
    vpPlaceBase(gs, 2, 1, 80, 40);

    vpPlaceTank(gs, 1, 150, 150);
    vpPlaceTank(gs, 2, 170, 170);
    vpPlaceTank(gs, 3, 160, 160);

    *outGs = gs;
    return sim;
}

/* The build every case makes: our tank whole, at the square above. The caller
 * places the tank rect now, so the block is spelled out here rather than named
 * by a half-width. */
static int vpBuild(GameSim *gs, const OverviewViewInputs *in,
                   OverviewRect *out) {
    OverviewRect tank; /* The 29x29 the tank has always had */

    memset(&tank, 0, sizeof(tank));
    tank.alpha  = 255;
    tank.left   = VP_TANK_MX - OVERVIEW_TANK_HALF;
    tank.top    = VP_TANK_MY - OVERVIEW_TANK_HALF;
    tank.right  = VP_TANK_MX + OVERVIEW_TANK_HALF;
    tank.bottom = VP_TANK_MY + OVERVIEW_TANK_HALF;
    return overviewMapBuildRegions(gs, 0, in, &tank, out,
                                   OVERVIEW_MAX_REGIONS);
}

/* 1. The rules a server ships with produce the region set the overview has
 *    always had: the tank's block and the pillboxes the player can view
 *    through, nothing else, every one of them fully live. Bases are off, and
 *    allied tanks earn a block only once there is a live ally to watch — which
 *    is the whole of what the shipped configuration adds. */
int run_overview_policy_baseline(void) {
    GameSim *gs = NULL;
    ServerSim *sim = vpMakeWorld(&gs);
    UT_ASSERT_MSG(sim != NULL, "vpMakeWorld returned NULL");

    OverviewViewInputs in;
    OverviewRect out[OVERVIEW_MAX_REGIONS];
    int n;
    int i;

    overviewViewInputsDefaults(&in);
    UT_ASSERT_MSG(in.policy[viewCategoryPill] == viewPolicyAlways &&
                      in.policy[viewCategoryBase] == viewPolicyOff &&
                      in.policy[viewCategoryAlly] == viewPolicyAlways,
                  "the shipped rules are pill always, base off, ally always; "
                  "got %d, %d, %d",
                  (int)in.policy[viewCategoryPill],
                  (int)in.policy[viewCategoryBase],
                  (int)in.policy[viewCategoryAlly]);

    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 3,
                  "the shipped rules over a world of allied bases and live "
                  "allies gave %d regions, expected the tank and two pills", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");
    ASSERT_BLOCK(out[1], 200, 50, OVERVIEW_PILL_HALF, 255, "our pill's block");
    ASSERT_BLOCK(out[2], 210, 60, OVERVIEW_PILL_HALF, 255,
                 "the allied pill's block");
    for (i = 0; i < n; i++) {
        UT_ASSERT_MSG(out[i].alpha == 255,
                      "region %d came out at alpha %u with nothing decaying",
                      i, (unsigned)out[i].alpha);
    }

    /* The allied tanks were there all along; what was missing was any record
     * of one being alive. That is what the ally rule adds. */
    in.allyViewable = (PlayerBitMap)((1u << 1) | (1u << 2) | (1u << 3));
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 5, "two live allies gave %d regions, expected 5", n);
    ASSERT_BLOCK(out[3], 150, 150, OVERVIEW_PILL_HALF, 255,
                 "the first ally's block");
    ASSERT_BLOCK(out[4], 160, 160, OVERVIEW_PILL_HALF, 255,
                 "the second ally's block");

    serverSimDestroy(sim);
    return 0;
}

/* 2. What each policy grants, and in what order. Always sweeps a category,
 *    key grants the one item the player is watching, and off grants nothing;
 *    an item the player could not watch anyway never appears whatever the
 *    policy says. */
int run_overview_policy_categories(void) {
    GameSim *gs = NULL;
    ServerSim *sim = vpMakeWorld(&gs);
    UT_ASSERT_MSG(sim != NULL, "vpMakeWorld returned NULL");

    OverviewViewInputs in;
    OverviewRect out[OVERVIEW_MAX_REGIONS];
    int n;

    /* Everything on always, every ally alive: the whole set, in the order the
     * farewell stamp replays — tank, pills, bases, allied tanks, each kind in
     * ascending index. The enemy pill, the enemy base and the enemy tank are
     * not the player's to watch and are absent throughout. */
    overviewViewInputsDefaults(&in);
    in.policy[viewCategoryBase] = viewPolicyAlways;
    in.allyViewable = (PlayerBitMap)((1u << 1) | (1u << 2) | (1u << 3));

    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 7, "everything on always gave %d regions, expected 7", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");
    ASSERT_BLOCK(out[1], 200, 50, OVERVIEW_PILL_HALF, 255, "pill 0's block");
    ASSERT_BLOCK(out[2], 210, 60, OVERVIEW_PILL_HALF, 255, "pill 1's block");
    ASSERT_BLOCK(out[3], 40, 40, OVERVIEW_PILL_HALF, 255, "base 0's block");
    ASSERT_BLOCK(out[4], 80, 40, OVERVIEW_PILL_HALF, 255, "base 2's block");
    ASSERT_BLOCK(out[5], 150, 150, OVERVIEW_PILL_HALF, 255, "ally 1's block");
    ASSERT_BLOCK(out[6], 160, 160, OVERVIEW_PILL_HALF, 255, "ally 3's block");

    /* Everything off: the player's own screen and nothing else. */
    in.policy[viewCategoryPill] = viewPolicyOff;
    in.policy[viewCategoryBase] = viewPolicyOff;
    in.policy[viewCategoryAlly] = viewPolicyOff;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "everything off gave %d regions, expected the tank "
                  "block on its own", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");

    /* Everything on key. Until the player is watching something there is
     * nothing to grant; then it is the one item, whichever kind it is, and the
     * tank block goes for as long as they watch it — key is one view at a
     * time, so the item's block replaces the tank's rather than joining it. */
    in.policy[viewCategoryPill] = viewPolicyKey;
    in.policy[viewCategoryBase] = viewPolicyKey;
    in.policy[viewCategoryAlly] = viewPolicyKey;
    in.viewKind = VIEW_KIND_TANK;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key with the player on the tank view gave %d "
                  "regions, expected the tank block on its own", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");

    in.viewKind = VIEW_KIND_PILL;
    in.viewTarget = 1;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key on pill 1 gave %d regions, expected the watched "
                  "pill's block on its own", n);
    ASSERT_BLOCK(out[0], 210, 60, OVERVIEW_PILL_HALF, 255,
                 "the watched pill's block");

    in.viewKind = VIEW_KIND_BASE;
    in.viewTarget = 2;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key on base 2 gave %d regions, expected the watched "
                  "base's block on its own", n);
    ASSERT_BLOCK(out[0], 80, 40, OVERVIEW_PILL_HALF, 255,
                 "the watched base's block");

    in.viewKind = VIEW_KIND_ALLY;
    in.viewTarget = 3;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key on ally 3 gave %d regions, expected the watched "
                  "ally's block on its own", n);
    ASSERT_BLOCK(out[0], 160, 160, OVERVIEW_PILL_HALF, 255,
                 "the watched ally's block");

    /* A claim on something that is not the player's to watch grants nothing —
     * the enemy's base, and the enemy's tank — and the tank block is still
     * there, because nothing took its place. */
    in.viewKind = VIEW_KIND_BASE;
    in.viewTarget = 1;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key on an enemy base gave %d regions, expected the "
                  "tank block on its own", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");

    in.viewKind = VIEW_KIND_ALLY;
    in.viewTarget = 2;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "key on an enemy tank gave %d regions, expected the "
                  "tank block on its own", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");

    /* Only key does this. The same claim with the category on always leaves
     * the tank block where it is and lights every pill besides. */
    in.policy[viewCategoryPill] = viewPolicyAlways;
    in.viewKind = VIEW_KIND_PILL;
    in.viewTarget = 1;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 3, "always while watching pill 1 gave %d regions, "
                  "expected the tank block and both viewable pills", n);
    ASSERT_BLOCK(out[0], VP_TANK_MX, VP_TANK_MY, OVERVIEW_TANK_HALF, 255,
                 "the tank block");

    serverSimDestroy(sim);
    return 0;
}

/* 3. A decay window from one end to the other: nothing before the player has
 *    been near the item, full brightness once they have, fading over the last
 *    VIEW_DECAY_FADE_SECS of the window and gone past it, and full again the
 *    moment they come back. Bases and allied tanks decay the same way. */
int run_overview_policy_decay(void) {
    GameSim *gs = NULL;
    ServerSim *sim = vpMakeWorld(&gs);
    UT_ASSERT_MSG(sim != NULL, "vpMakeWorld returned NULL");

    OverviewViewInputs in;
    OverviewRect out[OVERVIEW_MAX_REGIONS];
    uint32_t pillClock[MAX_PILLS];
    uint32_t baseClock[MAX_BASES];
    uint32_t allyClock[MAX_TANKS];
    uint32_t window;  /* Ticks a stamp stays good for */
    uint32_t fade;    /* Ticks of that window the fade takes */
    uint32_t age;     /* Ticks since the stamp */
    unsigned prev;    /* The alpha of the tick before */
    int n;

    memset(pillClock, 0, sizeof(pillClock));
    memset(baseClock, 0, sizeof(baseClock));
    memset(allyClock, 0, sizeof(allyClock));

    overviewViewInputsDefaults(&in);
    in.policy[viewCategoryPill] = viewPolicyDecay;
    in.policy[viewCategoryBase] = viewPolicyDecay;
    in.policy[viewCategoryAlly] = viewPolicyDecay;
    in.decaySecs[viewCategoryPill] = VIEW_DECAY_DEFAULT_SECS;
    in.decaySecs[viewCategoryBase] = VIEW_DECAY_DEFAULT_SECS;
    in.decaySecs[viewCategoryAlly] = VIEW_DECAY_DEFAULT_SECS;
    in.pillNearTick = pillClock;
    in.baseNearTick = baseClock;
    in.allyNearTick = allyClock;
    in.ticksPerSec = VP_TICKS_SEC;
    in.allyViewable = (PlayerBitMap)((1u << 1) | (1u << 2) | (1u << 3));

    window = (uint32_t)VIEW_DECAY_DEFAULT_SECS * VP_TICKS_SEC;
    fade = (uint32_t)VIEW_DECAY_FADE_SECS * VP_TICKS_SEC;
    UT_ASSERT_MSG(window > fade,
                  "this case needs a window longer than the fade (%u vs %u)",
                  (unsigned)window, (unsigned)fade);
    in.nowTick = window * 4; /* far enough in that no age underflows */

    /* Never been near anything: the player has their own screen and nothing
     * else, however many items they would otherwise be entitled to. */
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "clocks that have never been stamped gave %d "
                  "regions, expected the tank block on its own", n);

    /* The whole window for the pillbox, a tick at a time: full brightness
     * until the fade starts, never brighter than the tick before it from
     * there, down to nothing on the last tick, and gone once the window has
     * been passed. */
    prev = 256;
    for (age = 0; age <= window; age++) {
        unsigned want; /* What this tick should read */

        pillClock[0] = in.nowTick - age;
        n = vpBuild(gs, &in, out);
        UT_ASSERT_MSG(n == 2, "at %u ticks old the pill gave %d regions, "
                      "expected the tank and the pill", (unsigned)age, n);
        ASSERT_RECT(out[1], 200 - OVERVIEW_PILL_HALF, 50 - OVERVIEW_PILL_HALF,
                    200 + OVERVIEW_PILL_HALF, 50 + OVERVIEW_PILL_HALF);

        want = out[1].alpha;
        UT_ASSERT_MSG(want <= prev,
                      "the fade brightened at %u ticks old: %u after %u",
                      (unsigned)age, want, prev);
        if (window - age >= fade) {
            UT_ASSERT_MSG(want == 255,
                          "at %u ticks old, %u before the fade starts, alpha "
                          "is %u", (unsigned)age,
                          (unsigned)(window - age - fade), want);
        } else if (age == window) {
            UT_ASSERT_MSG(want == 0,
                          "the last tick of the window has alpha %u", want);
        } else {
            UT_ASSERT_MSG(want > 0 && want < 255,
                          "inside the fade at %u ticks old alpha is %u",
                          (unsigned)age, want);
        }
        prev = want;
    }

    pillClock[0] = in.nowTick - (window + 1);
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 1, "a window that has run out gave %d regions, expected "
                  "the tank block on its own", n);

    /* Driving back past it starts the window over. */
    pillClock[0] = in.nowTick;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 2 && out[1].alpha == 255,
                  "coming back gave %d regions at alpha %u, expected 2 at 255",
                  n, (unsigned)out[1].alpha);

    /* Bases and allied tanks run on their own clocks, in the same order as
     * ever: a stamped base after the pills, a stamped ally after the bases. */
    pillClock[0] = 0;
    baseClock[2] = in.nowTick;
    allyClock[3] = in.nowTick - (window - fade / 2);
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 3, "a stamped base and a stamped ally gave %d regions, "
                  "expected 3", n);
    ASSERT_BLOCK(out[1], 80, 40, OVERVIEW_PILL_HALF, 255,
                 "the base the player drove past");
    ASSERT_RECT(out[2], 160 - OVERVIEW_PILL_HALF, 160 - OVERVIEW_PILL_HALF,
                160 + OVERVIEW_PILL_HALF, 160 + OVERVIEW_PILL_HALF);
    UT_ASSERT_MSG(out[2].alpha > 0 && out[2].alpha < 255,
                  "the ally is half way through its fade at alpha %u",
                  (unsigned)out[2].alpha);

    /* An item the player could not watch anyway earns nothing from having been
     * driven past: the clocks are stamped on proximity alone, and it is the
     * build that asks whose it is. */
    baseClock[1] = in.nowTick;
    allyClock[2] = in.nowTick;
    n = vpBuild(gs, &in, out);
    UT_ASSERT_MSG(n == 3, "stamping the enemy's base and tank gave %d regions, "
                  "expected the same 3", n);

    serverSimDestroy(sim);
    return 0;
}

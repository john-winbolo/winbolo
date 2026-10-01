/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * THREE SHOTS = GO THERE.
 *
 * Three of one player's shells that run their full range and die on the
 * SAME open square, FIRED inside SHOT_ORDER_WINDOW_TICKS of each other and
 * with a quiet second either side of them, are an order: the bots on that
 * player's team read "!goto <mx> <my> <sx> <sy>" — the square, then the
 * shooter's own square — and the nearest one goes there and holds. The
 * server spots the pattern; the brains decide who obeys, and none of that
 * is tested here.
 *
 * Every timing rule is on the FIRE tick, never on the landing, so these
 * tests say when each shell left the gun and when it died as two separate
 * numbers. The third shot only ARMS the order; the per-tick poll sends it a
 * quiet second later.
 *
 * What is tested is the detector, at the three functions the shell-expiry
 * callback and the scenario funnel's shell_expired op both drive:
 *
 *   three_shot_order_orders_allied_bots  — three on one open square put
 *       exactly one line in the allied bot's inbox a quiet second after the
 *       third was FIRED and not before, the line carries the shooter's own
 *       square, the sender is the shooter, and the enemy bot hears nothing
 *   three_shot_order_ignores_closed_ground — three on forest order nobody
 *   three_shot_order_needs_one_square     — two squares between them do not
 *   three_shot_order_window_and_reset     — a shot outside the window does
 *       not pair with the ones before it, the fourth shot after it starts a
 *       fresh count that does order, and the order clears the count
 *   three_shot_order_needs_quiet_before   — a shot fired 50 ticks before the
 *       first of the three stops it, and the same three left a clear second
 *       do order
 *   three_shot_order_quiet_counts_hits    — a shell that HIT something is
 *       still a shot fired, and breaks the quiet second the same way
 *   three_shot_order_needs_quiet_after    — a fourth shot fired 50 ticks
 *       after the third takes the armed order away, for good
 *   three_shot_order_quiet_after_sees_a_shell_still_flying
 *                                         — and it takes it away at the
 *       moment the gun went off, not when that shell lands: a fourth fired
 *       90 ticks after the third and still in the air when the poll comes
 *       round cancels the order all the same
 *   three_shot_order_quiet_before_sees_a_shell_still_flying
 *                                         — a shell fired one tick before
 *       the first of the three and slower to land than it is not missing
 *       from the log when the third lands, so the quiet second in front is
 *       not read as clear
 *
 * Most shots are posted through serverSimShotOrderShotFired and
 * serverSimShotOrderNote rather than by firing a gun: three full-range
 * shells landing on one chosen square is not a state a test could steer a
 * round into, and the expiry callback's own arm is the same two calls.
 *
 * The two "still flying" cases are the exception. They are about the moment
 * the server hears of a shell at all, so they put a real shell in the air
 * with shellsAddItem and never let it die — which is exactly the shell the
 * detector used to know nothing about.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"                /* the terrain codes */
#include "allience.h"
#include "bot_manager.h"           /* BotContext — the fixture patches bots[] */
#include "bolo_map.h"              /* mapSetPos — the square this test chooses */
#include "client_sim.h"
#include "client_sim_internal.h"
#include "game_sim.h"
#include "internal/messages.h"     /* messageInboxCount/Peek, BRAIN_INBOX_MSG_LEN */
#include "internal/shells.h"       /* shellsAddItem — a real shell leaving the gun */
#include "players.h"               /* PLAYER_FLAG_BOT */
#include "server_sim.h"
#include "server_sim_internal.h"   /* struct ServerSim — tick, shotOrder, botMgr */
#include "server/sim/server_sim_shared.h"  /* serverSimShotOrderNote */
#include "server_sim_scenario.h"    /* serverSimApplyScenarioOp — the script hook */
#include "test_harness.h"

/* The seats: the shooter is the human, one allied bot hears the order and
 * one enemy bot must not. */
#define TS_SHOOTER 0
#define TS_ALLY    1
#define TS_ENEMY   2

/* Two squares well inside Everard Island, one made open ground and one made
 * forest by hand so the test does not depend on what the map holds. A third
 * open square proves two squares between them are not one square. */
#define TS_OPEN_X   100
#define TS_OPEN_Y   100
#define TS_TREE_X   104
#define TS_TREE_Y   100
#define TS_OTHER_X  108
#define TS_OTHER_Y  100

/* HOW LONG A FULL-RANGE SHELL IS IN THE AIR, in SERVER ticks — worked out
 * rather than guessed, because the old "about half a second" was what hid a
 * fourth shot from the quiet second after the three.
 *
 * A tank fires with len = sightLen / 2 and sightLen tops out at GUNSIGHT_MAX
 * (14, tank.h), so len is at most 7 map squares. shellsAddItem sets the
 * shell's life to shellLifeTicks(7, shell_life 8, shell_start_add 5) =
 * 1 + 8*7 - 5 = 52, and shellsUpdate runs once per GAME tick, which is every
 * SECOND server tick (server_sim_tick.c simRunHalfStep). 52 * 2 = 104 server
 * ticks — LONGER than SHOT_ORDER_QUIET_TICKS (100). A fire log fed only by
 * shell deaths therefore could not see a fourth shot at all. */
#define TS_SHELL_MAX_LEN      7
#define TS_SHELL_FLIGHT_TICKS 104

/* And the SHORTEST shot, the same way. sightLen bottoms out at GUNSIGHT_MIN
 * (2), so len is 1: shellLifeTicks(1, 8, 5) = 1 + 8 - 5 = 4 game ticks, 8
 * server ticks in the air. A shell this short lands while the quiet second
 * after the third shot is still running, which is the only window in which
 * the poll can tell a server tick from a client's. */
#define TS_SHELL_MIN_LEN            1
#define TS_SHELL_SHORT_FLIGHT_TICKS 8

typedef struct {
    ServerSim *sim;
    ClientSim *ally;
    ClientSim *enemy;
} TsFixture;

static ClientSim *tsBotCs(BYTE slot) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    return cs;
}

/* A running sim with the shooter allied to one bot and not to the other.
 * The two bot ClientSims are patched straight into botMgr.bots[] the way
 * test_brain_internal_msg_routing.c does: the delivery path reads .active
 * and .cs alone, and botManagerAddBot would want a Lua VM and a brain. */
static int tsSetup(TsFixture *f) {
    GameSim *gs;
    players *plrs;

    memset(f, 0, sizeof(*f));
    f->sim = ut_make_running_sim("Shooter");
    if (f->sim == NULL) {
        fprintf(stderr, "ut_make_running_sim returned NULL\n");
        return 1;
    }
    serverSimAddPlayer(f->sim, TS_ALLY, "AllyBot", false);
    serverSimAddPlayer(f->sim, TS_ENEMY, "EnemyBot", false);

    gs   = serverSimGetGameSim(f->sim);
    plrs = &gs->plyrs;
    (*plrs)->item[TS_ALLY].clientFlags  |= PLAYER_FLAG_BOT;
    (*plrs)->item[TS_ENEMY].clientFlags |= PLAYER_FLAG_BOT;
    allienceAdd(&(*plrs)->item[TS_SHOOTER].allie, TS_ALLY);
    allienceAdd(&(*plrs)->item[TS_ALLY].allie, TS_SHOOTER);

    /* The squares the shots land on. mineClear TRUE so neither carries a
     * mine the terrain read would have to look past. */
    mapSetPos(gs, &gs->mp, TS_OPEN_X, TS_OPEN_Y, GRASS, FALSE, TRUE);
    mapSetPos(gs, &gs->mp, TS_OTHER_X, TS_OTHER_Y, ROAD, FALSE, TRUE);
    mapSetPos(gs, &gs->mp, TS_TREE_X, TS_TREE_Y, FOREST, FALSE, TRUE);

    f->ally  = tsBotCs(TS_ALLY);
    f->enemy = tsBotCs(TS_ENEMY);
    if (f->ally == NULL || f->enemy == NULL) {
        fprintf(stderr, "tsBotCs failed\n");
        return 1;
    }
    f->sim->botMgr.bots[TS_ALLY].active  = true;
    f->sim->botMgr.bots[TS_ALLY].cs      = f->ally;
    f->sim->botMgr.bots[TS_ENEMY].active = true;
    f->sim->botMgr.bots[TS_ENEMY].cs     = f->enemy;
    return 0;
}

static void tsTeardown(TsFixture *f) {
    if (f->sim != NULL) {
        f->sim->botMgr.bots[TS_ALLY].active  = false;
        f->sim->botMgr.bots[TS_ALLY].cs      = NULL;
        f->sim->botMgr.bots[TS_ENEMY].active = false;
        f->sim->botMgr.bots[TS_ENEMY].cs     = NULL;
    }
    if (f->ally)  clientSimDestroy(f->ally);
    if (f->enemy) clientSimDestroy(f->enemy);
    if (f->sim)   serverSimDestroy(f->sim);
}

/* One shell of the shooter's, FIRED on `fireTick` and dead over the centre
 * of a square with the sim standing at `now` — the two calls a real shell
 * death makes, in the order serverSimCbShellDeath makes them. */
static void tsShotAt(TsFixture *f, uint32_t now, uint32_t fireTick,
                     BYTE mx, BYTE my) {
    f->sim->tick = now;
    serverSimShotOrderShotFired(f->sim, TS_SHOOTER, fireTick);
    serverSimShotOrderNote(f->sim, TS_SHOOTER, fireTick,
                           (WORLD)(((WORLD)mx << TANK_SHIFT_MAPSIZE) + 128),
                           (WORLD)(((WORLD)my << TANK_SHIFT_MAPSIZE) + 128));
}

/* The short form: a shell fired and landed on the same tick. A real shell is
 * TS_SHELL_FLIGHT_TICKS in the air, but only the fire tick is read, so the
 * two numbers are separated only where a test is about that gap. */
static void tsShot(TsFixture *f, uint32_t tick, BYTE mx, BYTE my) {
    tsShotAt(f, tick, tick, mx, my);
}

/* A shell of the shooter's that HIT something: a shot fired, with no landing
 * to offer the detector. */
static void tsHit(TsFixture *f, uint32_t tick, uint32_t fireTick) {
    f->sim->tick = tick;
    serverSimShotOrderShotFired(f->sim, TS_SHOOTER, fireTick);
}

/* A REAL shell of the shooter's, leaving the gun on `fireTick` and left in
 * the air. Nothing else in this file fires a gun, and this one does for one
 * reason: the fire log is what these two cases are about, and a shell that
 * is still flying is the one the server used to know nothing of.
 *
 * shellsAddItem stamps the shell with sim->fireInputTick, which is what the
 * server sets from the input packet it is applying, so the tick is set the
 * same way here. The shell is never updated afterwards, so it never lands
 * and never reports itself — which is the whole point. */
static void tsFireShell(TsFixture *f, uint32_t fireTick) {
    GameSim *gs = serverSimGetGameSim(f->sim);
    WORLD    wx = 0, wy = 0;

    f->sim->tick = fireTick;
    serverSimGetTankState(f->sim, TS_SHOOTER, &wx, &wy);
    gs->fireInputTick = fireTick;
    shellsAddItem(gs, &gs->shs, wx, wy, (TURNTYPE)0.0,
                  (TURNTYPE)TS_SHELL_MAX_LEN, TS_SHOOTER, NEUTRAL, DMG_NO_PILL, FALSE);
    gs->fireInputTick = 0;
}

/* The same, with the CLIENT's input tick said separately from the server tick
 * the shell leaves the gun on — which is the whole point of these two. Returns
 * the server fire tick the shell was stamped with, read back off the shell
 * itself (shellsAddItem prepends, so the newest is at the head of the list).
 *
 * Nothing removes the shell afterwards. It never gets updated either, so it
 * never moves, never lands and never reports itself; it just sits in the list
 * for the rest of the case. */
static uint32_t tsFireShellAs(TsFixture *f, uint32_t serverTick,
                              uint32_t clientFireTick, int len) {
    GameSim *gs = serverSimGetGameSim(f->sim);
    WORLD    wx = 0, wy = 0;

    f->sim->tick = serverTick;
    serverSimGetTankState(f->sim, TS_SHOOTER, &wx, &wy);
    gs->fireInputTick = clientFireTick;
    shellsAddItem(gs, &gs->shs, wx, wy, (TURNTYPE)0.0, (TURNTYPE)len,
                  TS_SHOOTER, NEUTRAL, DMG_NO_PILL, FALSE);
    gs->fireInputTick = 0;
    return (gs->shs != NULL) ? gs->shs->serverFireTick : 0u;
}

/* That shell's death, over the centre of a square, through the REAL callback
 * — so the test drives the same two arguments a landing hands the detector:
 * the client's fireTick (which goes on the wire and nowhere else) and the
 * server fire tick the shell was stamped with (which every rule reads). */
static void tsLandShell(TsFixture *f, uint32_t landTick,
                        uint32_t clientFireTick, uint32_t serverFireTick,
                        BYTE mx, BYTE my) {
    f->sim->tick = landTick;
    serverSimCbShellDeath(f->sim, clientFireTick, serverFireTick, TS_SHOOTER,
                          (WORLD)(((WORLD)mx << TANK_SHIFT_MAPSIZE) + 128),
                          (WORLD)(((WORLD)my << TANK_SHIFT_MAPSIZE) + 128),
                          SHELL_OUTCOME_EXPIRED);
}

/* The server's per-tick poll, with the sim standing at `tick`. This is what
 * sends an armed order once its quiet second is up. */
static void tsPollAt(TsFixture *f, uint32_t tick) {
    f->sim->tick = tick;
    serverSimShotOrderTick(f->sim);
}

static int tsInbox(ClientSim *cs) {
    return messageInboxCount(clientSimGetMessages(cs));
}

/* The body of inbox entry `i`, which the inbox holds as a Pascal string. */
static BYTE tsInboxBody(ClientSim *cs, int i, char *out, size_t cap) {
    char   pbuf[BRAIN_INBOX_MSG_LEN];
    BYTE   from = messageInboxPeek(clientSimGetMessages(cs), i, pbuf);
    size_t plen = (size_t)(unsigned char)pbuf[0];
    if (plen >= cap) plen = cap - 1;
    memcpy(out, pbuf + 1, plen);
    out[plen] = '\0';
    return from;
}

/* Three shells on one open square, inside the window, with a quiet second
 * either side — the whole rule, from the first shot to the line. */
int run_three_shot_order_orders_allied_bots(void) {
    TsFixture f;
    char      body[BRAIN_INBOX_MSG_LEN];
    char      want[BRAIN_INBOX_MSG_LEN];
    WORLD     swx = 0, swy = 0;
    BYTE      from;

    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "one shell ordered somebody: the inbox holds %d",
                  tsInbox(f.ally));
    tsShot(&f, 150, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two shells ordered somebody: the inbox holds %d",
                  tsInbox(f.ally));
    tsShot(&f, 200, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the third shell sent the order at once: %d entries. It "
                  "only arms it — the quiet second after it has to pass "
                  "first", tsInbox(f.ally));

    /* One tick short of the quiet second, and nothing has been said. */
    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS - 1);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the order went out %d ticks after the third shot, a tick "
                  "early: %d entries",
                  SHOT_ORDER_QUIET_TICKS - 1, tsInbox(f.ally));

    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "a quiet second after the third shot the inbox holds %d "
                  "entries, expected 1", tsInbox(f.ally));

    /* The line names the square AND the shooter's own square: the range rule
       the brains apply is the shooter's view, not the target's. */
    from = tsInboxBody(f.ally, 0, body, sizeof(body));
    serverSimGetTankState(f.sim, TS_SHOOTER, &swx, &swy);
    snprintf(want, sizeof(want), "!goto %d %d %u %u", TS_OPEN_X, TS_OPEN_Y,
             (unsigned)(swx >> TANK_SHIFT_MAPSIZE),
             (unsigned)(swy >> TANK_SHIFT_MAPSIZE));
    UT_ASSERT_MSG(strcmp(body, want) == 0,
                  "the line reads \"%s\", expected \"%s\"", body, want);
    UT_ASSERT_MSG(from == TS_SHOOTER,
                  "the line is from slot %u, expected the shooter (%d) — the "
                  "brain's team check reads the sender",
                  (unsigned)from, TS_SHOOTER);

    /* The other team's bot is not the shooter's ally, so it hears nothing. */
    UT_ASSERT_MSG(tsInbox(f.enemy) == 0,
                  "the enemy bot heard the order: %d entries",
                  tsInbox(f.enemy));

    /* The order clears the count: three more shells are needed for a
       second one, not one. */
    tsShot(&f, 400, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 400 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "a fourth shell ordered again straight away: %d entries",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* Forest is not open ground, and neither is what a shell would have hit. */
int run_three_shot_order_ignores_closed_ground(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, TS_TREE_X, TS_TREE_Y);
    tsShot(&f, 150, TS_TREE_X, TS_TREE_Y);
    tsShot(&f, 200, TS_TREE_X, TS_TREE_Y);
    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "three shells on forest ordered somebody: %d entries",
                  tsInbox(f.ally));

    /* And the forest shells did not even count as landings: three on an open
       square are still the first three. They are left a clear second after
       the forest ones, because a forest shell was still a shot FIRED and the
       quiet second in front of the first counts every shell. */
    tsShot(&f, 400, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 450, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 450 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two open-ground shells after three forest ones ordered "
                  "somebody: %d entries", tsInbox(f.ally));
    tsShot(&f, 500, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 500 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "three open-ground shells left %d entries, expected 1",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* Three shells, two squares: an order names one square or it names none. */
int run_three_shot_order_needs_one_square(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, TS_OPEN_X,  TS_OPEN_Y);
    tsShot(&f, 120, TS_OPEN_X,  TS_OPEN_Y);
    tsShot(&f, 140, TS_OTHER_X, TS_OTHER_Y);
    tsPollAt(&f, 140 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two squares between three shells ordered somebody: %d "
                  "entries", tsInbox(f.ally));

    /* Alternating between two open squares never makes three of either. */
    tsShot(&f, 160, TS_OPEN_X,  TS_OPEN_Y);
    tsShot(&f, 180, TS_OTHER_X, TS_OTHER_Y);
    tsShot(&f, 200, TS_OPEN_X,  TS_OPEN_Y);
    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "alternating squares ordered somebody: %d entries",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* The window, and what a shot past it does to the count. */
int run_three_shot_order_window_and_reset(void) {
    TsFixture f;
    uint32_t  window = (uint32_t)SHOT_ORDER_WINDOW_TICKS;
    if (tsSetup(&f) != 0) return 1;

    /* Three on the square, but the first and the last a tick more than the
       window apart: slow fire at a wreck, not an order. The middle one is
       left a clear second after the first so that only the window is being
       asked about here. */
    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 250, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 100 + window + 1, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 100 + window + 1 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "three shells %u ticks apart ordered somebody (the window "
                  "is %u): %d entries", window + 1, window, tsInbox(f.ally));

    /* A fourth shell now makes three inside the window with the two most
       recent, which IS an order: the count runs on rather than starting
       over, and the window is what decides. */
    tsShot(&f, 100 + window + 2, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 100 + window + 2 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "the fourth shell left %d entries, expected the 1 the three "
                  "inside the window order", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE QUIET SECOND IN FRONT. Three shells on one square are an order only
 * when the player fired nothing in the second before the first of them. */
int run_three_shot_order_needs_quiet_before(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    /* A shell somewhere else, 50 ticks before the burst starts. */
    tsShot(&f, 300, TS_OTHER_X, TS_OTHER_Y);
    tsShot(&f, 350, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 400, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 450, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 450 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a shot 50 ticks before the first of the three still "
                  "ordered somebody: %d entries", tsInbox(f.ally));

    /* The same three, left a clear second this time, DO order: it was the
       quiet second that stopped them and nothing else. */
    tsShot(&f, 700, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 750, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 800, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 800 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "three shells with a clear second in front left %d "
                  "entries, expected 1", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* A shell that HIT something is still a shot fired, so it breaks the quiet
 * second exactly as a full-range one does. */
int run_three_shot_order_quiet_counts_hits(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    /* Fired on 300 and stopped by a wall on 340: no landing to offer the
       detector, but the trigger was pulled 50 ticks before the burst. */
    tsHit(&f, 340, 300);
    tsShot(&f, 350, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 400, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 450, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 450 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a shell that hit something did not count as a shot fired: "
                  "%d entries", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE QUIET SECOND AFTER. The third shot arms the order; a fourth fired
 * inside the second that follows takes it away again. */
int run_three_shot_order_needs_quiet_after(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 150, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 200, TS_OPEN_X, TS_OPEN_Y);

    /* A fourth shell, fired 50 ticks after the third. It is heard of when it
       dies, which is what a real one does. */
    tsHit(&f, 280, 250);
    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a fourth shot inside the quiet second left the order "
                  "standing: %d entries", tsInbox(f.ally));

    /* And it stays gone: the poll does not find it again on a later tick. */
    tsPollAt(&f, 200 + 4 * SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the cancelled order turned up later anyway: %d entries",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE QUIET SECOND AFTER, MEASURED AT THE GUN. The fourth shell is fired 90
 * ticks after the third and is still in the air when the poll comes round on
 * tick 300 — so nothing about its death can help. The order has to be gone
 * by then, because the trigger was pulled inside the quiet second. */
int run_three_shot_order_quiet_after_sees_a_shell_still_flying(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 150, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 200, TS_OPEN_X, TS_OPEN_Y);

    /* Fired on 290 — ten ticks before the quiet second is up — and a whole
       TS_SHELL_FLIGHT_TICKS from landing, which is 104 ticks and so lands on
       394, long after the poll at 300. Nothing about its death can help. */
    tsFireShell(&f, 290);

    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a fourth shell fired 90 ticks after the third but not yet "
                  "landed left the order standing: %d entries. The quiet "
                  "second is measured at the gun, so the fire has to be "
                  "recorded when the shell is created",
                  tsInbox(f.ally));

    /* Its death, a full flight later, is the same fire tick over again and
       must change nothing either way. */
    tsHit(&f, 290 + TS_SHELL_FLIGHT_TICKS, 290);
    tsPollAt(&f, 200 + 4 * SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the cancelled order turned up once the fourth shell "
                  "landed: %d entries", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE QUIET SECOND IN FRONT, MEASURED AT THE GUN. A shell fired one tick
 * before the first of the three takes longer to land than any of them, so
 * when the third lands on tick 200 it has still not died. The run must be
 * blocked all the same: the player pulled the trigger inside the quiet
 * second in front, which is the only question the rule asks. */
int run_three_shot_order_quiet_before_sees_a_shell_still_flying(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    /* Fired on 99, TS_SHELL_FLIGHT_TICKS to travel, so it is still 3 ticks
       from landing when the third of the three dies on 200. */
    tsFireShell(&f, 99);

    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 150, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 200, TS_OPEN_X, TS_OPEN_Y);

    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a shell fired one tick before the first of the three, and "
                  "still in the air when the third landed, did not break the "
                  "quiet second in front: %d entries", tsInbox(f.ally));

    /* The same three with nothing in front of them DO order, so it was the
       flying shell that stopped the first run and not the fixture. */
    tsShot(&f, 700, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 750, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 800, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 800 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "three shells with a clear second in front left %d "
                  "entries, expected 1", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE CLOCK IS THE SERVER'S. A MID-ROUND JOINER.
 *
 * A shell carries two ticks. The client's own input-tick counter rides with
 * it so the firing client can match the death to its predicted shell, and
 * that counter starts at zero when a client connects — a player who joins an
 * hour into a round is on tick 1 while the server is on tick 360000.
 *
 * Read the detector's rules on THAT number and everything goes wrong at once:
 * the burst looks like it happened at the dawn of the round, the poll's
 * "sim->tick >= armFireTick + 100" is true on the very next tick, and the
 * quiet second after the third shot is skipped entirely. The order fires at
 * once, and a fourth shot inside that second cannot take it back because it
 * has already gone.
 *
 * Bots never showed it: botManagerTick stamps their input packets with
 * serverSimGetTick, so their counter IS the server's.
 *
 * Here the joiner's counter says 1, 2, 3 while the server is on 1000, 1050
 * and 1100. The order must wait until 1100 + 100 and not a tick sooner. */
int run_three_shot_order_uses_the_server_tick(void) {
    TsFixture f;
    uint32_t  sft[3];
    int       i;

    if (tsSetup(&f) != 0) return 1;

    /* Three shells, fired on server ticks 1000/1050/1100 by a client whose
       own counter is 1/2/3. Short-range shots, so each lands 8 ticks after it
       was fired — WHILE the quiet second after the third is still running,
       which is the only window in which the poll can tell the two clocks
       apart. */
    sft[0] = tsFireShellAs(&f, 1000, 1, TS_SHELL_MIN_LEN);
    sft[1] = tsFireShellAs(&f, 1050, 2, TS_SHELL_MIN_LEN);
    sft[2] = tsFireShellAs(&f, 1100, 3, TS_SHELL_MIN_LEN);

    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(sft[i] == (uint32_t)(1000 + 50 * i),
                      "shell %d was stamped with server fire tick %u, expected "
                      "%u — shellsAddItem must take the tick from the server, "
                      "not from the client's packet",
                      i, (unsigned)sft[i], (unsigned)(1000 + 50 * i));
    }

    tsLandShell(&f, 1000 + TS_SHELL_SHORT_FLIGHT_TICKS, 1, sft[0],
                TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1050 + TS_SHELL_SHORT_FLIGHT_TICKS, 2, sft[1],
                TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1100 + TS_SHELL_SHORT_FLIGHT_TICKS, 3, sft[2],
                TS_OPEN_X, TS_OPEN_Y);

    /* Read on the client's counter the third shot was "fired" on tick 3, so
       this poll — 92 ticks before the quiet second is up — would think it was
       long over and send the order at once. */
    tsPollAt(&f, 1100 + TS_SHELL_SHORT_FLIGHT_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the order went out the moment the third shell landed: %d "
                  "entries. The detector is reading the client's tick counter, "
                  "which for a mid-round joiner is a handful of ticks and puts "
                  "the quiet second in the past", tsInbox(f.ally));

    tsPollAt(&f, 1100 + SHOT_ORDER_QUIET_TICKS - 1);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the order went out a tick early: %d entries",
                  tsInbox(f.ally));

    tsPollAt(&f, 1100 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "a quiet second after the third shot LEFT THE GUN the inbox "
                  "holds %d entries, expected 1", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE QUIET SECOND AFTER, FOR A MID-ROUND JOINER.
 *
 * Same three shots, and a fourth fired 50 server ticks after the third. On
 * server ticks that is plainly inside the quiet second and takes the armed
 * order away. Read on the client's counter the order would already have gone
 * out on the tick after the third landed, and there would be nothing left to
 * take. The test is that the order never goes out at all. */
int run_three_shot_order_server_tick_quiet_after(void) {
    TsFixture f;
    uint32_t  sft[3], fourth;

    if (tsSetup(&f) != 0) return 1;

    sft[0] = tsFireShellAs(&f, 1000, 1, TS_SHELL_MIN_LEN);
    sft[1] = tsFireShellAs(&f, 1050, 2, TS_SHELL_MIN_LEN);
    sft[2] = tsFireShellAs(&f, 1100, 3, TS_SHELL_MIN_LEN);
    tsLandShell(&f, 1000 + TS_SHELL_SHORT_FLIGHT_TICKS, 1, sft[0], TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1050 + TS_SHELL_SHORT_FLIGHT_TICKS, 2, sft[1], TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1100 + TS_SHELL_SHORT_FLIGHT_TICKS, 3, sft[2], TS_OPEN_X, TS_OPEN_Y);

    /* The fourth: server tick 1150, the client's own counter 4, and a FULL
       range so it is still in the air (until 1254) when the poll comes round
       on 1200. The fire log has to have heard of it at the gun. */
    fourth = tsFireShellAs(&f, 1150, 4, TS_SHELL_MAX_LEN);
    UT_ASSERT_MSG(fourth == 1150,
                  "the fourth shell was stamped %u, expected 1150",
                  (unsigned)fourth);

    tsPollAt(&f, 1100 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a fourth shot 50 server ticks after the third left the "
                  "order standing: %d entries", tsInbox(f.ally));

    tsPollAt(&f, 1100 + 4 * SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "the cancelled order turned up later anyway: %d entries",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* A MODIFIED CLIENT CANNOT PICK ITS OWN TICK.
 *
 * Nothing stops a client putting any 32-bit number it likes in an
 * InputPacket's tick field. A fireTick near the top of the range, read as a
 * fire time, would make the quiet second in front of the burst unmeasurable
 * (the lookback clamps to 0 and the whole log falls "before" it) and the
 * poll's comparison against sim->tick never true.
 *
 * The three shots below carry 0xFFFFFFF0, 0xFFFFFFF1 and 0xFFFFFFF2 and are
 * fired on ordinary server ticks 1000, 1050 and 1100. They must behave
 * exactly like any other three: no order before 1200, one order on 1200. */
int run_three_shot_order_ignores_a_forged_fire_tick(void) {
    TsFixture f;
    uint32_t  sft[3];
    const uint32_t forged = 0xFFFFFFF0u;

    if (tsSetup(&f) != 0) return 1;

    sft[0] = tsFireShellAs(&f, 1000, forged,      TS_SHELL_MIN_LEN);
    sft[1] = tsFireShellAs(&f, 1050, forged + 1u, TS_SHELL_MIN_LEN);
    sft[2] = tsFireShellAs(&f, 1100, forged + 2u, TS_SHELL_MIN_LEN);

    UT_ASSERT_MSG(sft[2] == 1100,
                  "a shell whose packet said tick %u was stamped %u, expected "
                  "the server's 1100 — nothing a client sends may become the "
                  "tick a server rule is measured on",
                  (unsigned)(forged + 2u), (unsigned)sft[2]);

    tsLandShell(&f, 1000 + TS_SHELL_SHORT_FLIGHT_TICKS, forged,      sft[0],
                TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1050 + TS_SHELL_SHORT_FLIGHT_TICKS, forged + 1u, sft[1],
                TS_OPEN_X, TS_OPEN_Y);
    tsLandShell(&f, 1100 + TS_SHELL_SHORT_FLIGHT_TICKS, forged + 2u, sft[2],
                TS_OPEN_X, TS_OPEN_Y);

    /* Read on the forged number the arm would sit at 0xFFFFFFF2, which no
       sim->tick ever reaches, and the order would never go out at all. */
    tsPollAt(&f, 1100 + SHOT_ORDER_QUIET_TICKS - 1);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "a forged fire tick got the order out early: %d entries",
                  tsInbox(f.ally));

    tsPollAt(&f, 1100 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "the three shots did not order at all: %d entries, expected "
                  "1", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* OFF THE MAP IS NOT OPEN GROUND.
 *
 * mapGetPos answers DEEP_SEA for a square the map does not hold, and deep sea
 * is open ground as far as a shell is concerned — so the whole border outside
 * the playable band read as somewhere an order could be dropped. Nothing can
 * ever stand there.
 *
 * Square (2, 2) is well outside MAP_MINE_EDGE_LEFT / MAP_MINE_EDGE_TOP (20).
 * Three full-range shells on it must order nobody, and must not even count as
 * landings. */
int run_three_shot_order_rejects_off_map_squares(void) {
    TsFixture f;
    if (tsSetup(&f) != 0) return 1;

    tsShot(&f, 100, 2, 2);
    tsShot(&f, 150, 2, 2);
    tsShot(&f, 200, 2, 2);
    tsPollAt(&f, 200 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "three shells on an off-map square ordered somebody: %d "
                  "entries. mapGetPos calls it deep sea, which the open-ground "
                  "test lets through", tsInbox(f.ally));

    /* And they did not count as landings either: three on a real open square,
       left a clear second after them, are still the first three. */
    tsShot(&f, 700, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 750, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 750 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two shells on an open square ordered somebody after three "
                  "off-map ones: %d entries — the off-map shells were counted",
                  tsInbox(f.ally));
    tsShot(&f, 800, TS_OPEN_X, TS_OPEN_Y);
    tsPollAt(&f, 800 + SHOT_ORDER_QUIET_TICKS);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "three shells on an open square left %d entries, expected 1",
                  tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

/* THE SCENARIO HOOK REFUSES AN OFF-MAP SQUARE TOO.
 *
 * game.shell_expired(p, x, y) is the only way a script can put a full-range
 * landing in front of the detector, and its two coordinates are BYTEs — the
 * Lua arm's own check passes anything 0..255. Square (2, 2) is outside the
 * playable band (MAP_MINE_EDGE_LEFT / _TOP are 20) and reads back as deep sea,
 * so without a bounds test of its own the op would happily count three
 * landings on a square no tank can ever reach.
 *
 * SCN_OP_BAD_SQUARE is the answer every other op with coordinates gives. */
int run_three_shot_order_scenario_refuses_off_map(void) {
    TsFixture   f;
    ScenarioOp  op;
    ScnOpResult r;

    if (tsSetup(&f) != 0) return 1;

    memset(&op, 0, sizeof(op));
    op.type                        = SCN_OP_SHELL_EXPIRED;
    op.u.shellExpired.slot         = TS_SHOOTER;
    op.u.shellExpired.x            = 2;
    op.u.shellExpired.y            = 2;
    op.u.shellExpired.haveFireTick = true;
    op.u.shellExpired.fireTick     = 100;

    r = serverSimApplyScenarioOp(f.sim, &op, NULL);
    UT_ASSERT_MSG(r == SCN_OP_BAD_SQUARE,
                  "shell_expired on the off-map square (2, 2) answered %d, "
                  "expected SCN_OP_BAD_SQUARE (%d)",
                  (int)r, (int)SCN_OP_BAD_SQUARE);

    /* A square the map really holds is still taken. */
    op.u.shellExpired.x = TS_OPEN_X;
    op.u.shellExpired.y = TS_OPEN_Y;
    r = serverSimApplyScenarioOp(f.sim, &op, NULL);
    UT_ASSERT_MSG(r == SCN_OP_OK,
                  "shell_expired on an ordinary open square answered %d, "
                  "expected SCN_OP_OK — the bounds test has taken too much",
                  (int)r);

    tsTeardown(&f);
    return 0;
}

/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
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
 *
 * The shots are posted through serverSimShotOrderShotFired and
 * serverSimShotOrderNote rather than by firing a gun: three full-range
 * shells landing on one chosen square is not a state a test could steer a
 * round into, and the expiry callback's own arm is the same two calls.
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
#include "players.h"               /* PLAYER_FLAG_BOT */
#include "server_sim.h"
#include "server_sim_internal.h"   /* struct ServerSim — tick, shotOrder, botMgr */
#include "server/sim/server_sim_shared.h"  /* serverSimShotOrderNote */
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
 * half a second in the air, but only the fire tick is read, so the two
 * numbers are separated only where a test is about that gap. */
static void tsShot(TsFixture *f, uint32_t tick, BYTE mx, BYTE my) {
    tsShotAt(f, tick, tick, mx, my);
}

/* A shell of the shooter's that HIT something: a shot fired, with no landing
 * to offer the detector. */
static void tsHit(TsFixture *f, uint32_t tick, uint32_t fireTick) {
    f->sim->tick = tick;
    serverSimShotOrderShotFired(f->sim, TS_SHOOTER, fireTick);
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

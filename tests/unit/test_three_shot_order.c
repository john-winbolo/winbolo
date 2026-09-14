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
 * SAME open square inside SHOT_ORDER_WINDOW_TICKS are an order: the bots
 * on that player's team read "!goto <mx> <my>" and the nearest one goes
 * there and holds. The server spots the pattern; the brains decide who
 * obeys, and none of that is tested here.
 *
 * What is tested is the detector, at the one function the shell-expiry
 * callback and the scenario funnel's shell_expired op both call:
 *
 *   three_shot_order_orders_allied_bots  — three on one open square put
 *       exactly one line in the allied bot's inbox, the sender is the
 *       shooter, and the enemy bot hears nothing
 *   three_shot_order_ignores_closed_ground — three on forest order nobody
 *   three_shot_order_needs_one_square     — two squares between them do not
 *   three_shot_order_window_and_reset     — a shot outside the window does
 *       not pair with the ones before it, the fourth shot after it starts a
 *       fresh count that does order, and the order clears the count
 *
 * The shots are posted through serverSimShotOrderNote rather than by
 * firing a gun: three full-range shells landing on one chosen square is
 * not a state a test could steer a round into, and the expiry callback's
 * own arm is one line that calls this on the expired outcome.
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

/* One shell of the shooter's, dead over the centre of a square at `tick`. */
static void tsShot(TsFixture *f, uint32_t tick, BYTE mx, BYTE my) {
    f->sim->tick = tick;
    serverSimShotOrderNote(f->sim, TS_SHOOTER,
                           (WORLD)(((WORLD)mx << TANK_SHIFT_MAPSIZE) + 128),
                           (WORLD)(((WORLD)my << TANK_SHIFT_MAPSIZE) + 128));
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

/* Three shells on one open square, inside the window. */
int run_three_shot_order_orders_allied_bots(void) {
    TsFixture f;
    char      body[BRAIN_INBOX_MSG_LEN];
    char      want[BRAIN_INBOX_MSG_LEN];
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
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "three shells on one open square left %d inbox entries, "
                  "expected 1", tsInbox(f.ally));

    from = tsInboxBody(f.ally, 0, body, sizeof(body));
    snprintf(want, sizeof(want), "!goto %d %d", TS_OPEN_X, TS_OPEN_Y);
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
    tsShot(&f, 250, TS_OPEN_X, TS_OPEN_Y);
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
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "three shells on forest ordered somebody: %d entries",
                  tsInbox(f.ally));

    /* And the forest shells did not even count: three on an open square
       right after them are still the first three. */
    tsShot(&f, 210, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 220, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two open-ground shells after three forest ones ordered "
                  "somebody: %d entries", tsInbox(f.ally));
    tsShot(&f, 230, TS_OPEN_X, TS_OPEN_Y);
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
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "two squares between three shells ordered somebody: %d "
                  "entries", tsInbox(f.ally));

    /* Alternating between two open squares never makes three of either. */
    tsShot(&f, 160, TS_OPEN_X,  TS_OPEN_Y);
    tsShot(&f, 180, TS_OTHER_X, TS_OTHER_Y);
    tsShot(&f, 200, TS_OPEN_X,  TS_OPEN_Y);
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
       window apart: slow fire at a wreck, not an order. */
    tsShot(&f, 100, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 100 + window / 2, TS_OPEN_X, TS_OPEN_Y);
    tsShot(&f, 100 + window + 1, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 0,
                  "three shells %u ticks apart ordered somebody (the window "
                  "is %u): %d entries", window + 1, window, tsInbox(f.ally));

    /* A fourth shell now makes three inside the window with the two most
       recent, which IS an order: the count runs on rather than starting
       over, and the window is what decides. */
    tsShot(&f, 100 + window + 2, TS_OPEN_X, TS_OPEN_Y);
    UT_ASSERT_MSG(tsInbox(f.ally) == 1,
                  "the fourth shell left %d entries, expected the 1 the three "
                  "inside the window order", tsInbox(f.ally));

    tsTeardown(&f);
    return 0;
}

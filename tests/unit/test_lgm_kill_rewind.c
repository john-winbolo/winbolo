/*
 * Killing a builder with lag: the server's rewind of the man when a shell goes
 * off near him, driven over the real loopback transport.
 *
 * ── The rewind rules this file tests (read from the code, not assumed) ────
 *
 *   When a shell goes off, shellsUpdate (src/bolo/shells.c, the collision
 *   branch ~419-458 and the ran-out-of-range branch ~503-542) asks every man
 *   whether it caught him. A man is only ever asked when a shell EXPLODES —
 *   hits a tank, a pill, a wall, or runs out of range. A shell that flies over
 *   him does nothing.
 *
 *   Which past position the man is tested at:
 *     - a TANK shell carries compensationTicks, stamped when it was fired from
 *       sim->lagCompTicks (shells.c:255). The man is tested where he stood that
 *       many frames before the explosion — the SHOOTER's lag.
 *     - a PILL shell (owner NEUTRAL, compensationTicks 0) uses
 *       sim->perPlayerCompTicks[man's owner] instead — the MAN OWNER's lag.
 *     - the past position comes from sim->lgmPosHistoryPtr[owner]
 *       (posHistoryGet, src/bolo/position_history.c). When there is none —
 *       rewind 0, ticksAgo past what the ring holds, or the man was dead or in
 *       his tank in that entry — the man's CURRENT position is used.
 *
 *   The kill test itself, lgmDeathCheckAtPosition (src/bolo/lgm.c ~1463): on
 *   open ground the man dies when the tested position is within
 *   MAP_SQUARE_MIDDLE (128 world units) of the explosion point. On a solid
 *   square (building, pill, base) he dies only when the tested position is on
 *   that same square. The explosion point is the shell's own position — only a
 *   hit on a pill, a base or a wall is moved to the square's centre; a shell
 *   that ran out of range or hit a tank goes off where it is.
 *
 *   The history ring (position_history.h): POSITION_HISTORY_SIZE = 16
 *   entries, one per game frame (20ms), written for every connected player in
 *   the world-systems block of serverSimTick (server_sim_tick.c ~812-819)
 *   AFTER lgmUpdate has moved the man and BEFORE pillsUpdate and shellsUpdate
 *   run. So "0 frames ago" is where the man is in the frame the shell goes
 *   off, and "N frames ago" is N frames of walking earlier.
 *
 *   How many frames (server_sim_tick.c):
 *     - tank shell: serverSimComputeLagCompTicks (~179-196), run for every
 *       input the server applies. When the input carries a viewTick (the
 *       server tick of the snapshot the client was showing, stamped by
 *       transport_udp_client.c:603 from clientState.prevAppliedServerTick),
 *       rewind = (serverTick - viewTick) / 2 — server ticks are half-steps, two
 *       to a frame. With no viewTick it falls back to
 *       (ping/2 + INTERP_BUFFER_MS) / 20.
 *     - pill shell: perPlayerCompTicks[p] = (ping/2 + INTERP_BUFFER_MS) / 20,
 *       recomputed every frame (~866-876) just before shellsUpdate. Ping only —
 *       it never uses a viewTick.
 *     - both are capped at LAG_COMP_MAX_TICKS = 14 frames (280ms,
 *       server_sim_internal.h:379). With a ring of 16 the cap is always inside
 *       the ring, so the "past the ring" fallback is not reachable from these
 *       two paths today.
 *
 * ── The four runs ─────────────────────────────────────────────────────────
 *
 *   lgm_kill_rewind_lagged_shooter  — the shooter is on a lagged link, the
 *       man's owner on a clean one. The shooter fires at where ITS CLIENT
 *       shows the man, leading him by the shell's flight time the way a player
 *       does. Expect: the man dies, and he dies because of the rewind — on his
 *       current position the shell misses by more than 128.
 *   lgm_kill_rewind_nolag_control   — the same move, both links clean. Expect:
 *       the man dies.
 *   lgm_kill_rewind_bounded         — lagged shooter again, but the shot goes
 *       where the man was well before the rewind window (it fires
 *       LKR_BOUNDED_LATE frames late). The server only goes back its rewind's
 *       worth of frames. Expect: the man lives.
 *   lgm_kill_rewind_pill_owner_lag  — a pillbox shoots a clean-link tank, and
 *       a lagged player's man walks across the line of fire just in front of
 *       that tank. Two shells are timed off the server's own clock: one lands
 *       where he IS but not where he WAS (he must live), one where he WAS but
 *       not where he IS (he must die). That pins the pill rule to the owner's
 *       perPlayerCompTicks both ways. test_lgm_under_fire.c (#343) shows the
 *       same rule changing a real fight's outcome but asserts only the
 *       outcome; this run asserts the geometry.
 *
 * ── The link ──────────────────────────────────────────────────────────────
 *
 *   "delay=80,jitter=0". 80ms each way is #343's delay, a normal online game.
 *   Jitter is dropped to 0 so every run measures the same ping and so the same
 *   rewind: with jitter the rewind wanders by a frame or two from run to run,
 *   and a frame is 16 world units of man walk. Measured at 80ms: the server
 *   reads a ping of 180-220ms for the lagged client (the round trip plus the
 *   client's own frame of queueing); the shooter's rewind comes out at 10-11
 *   frames — 160-176 world units of walk, more than the 128 kill radius, so
 *   the lagged shot tells rewind from no rewind; the pill rewind (ping only)
 *   is 5-6 frames. The clean link reads about 20ms and a rewind of 3.
 *
 *   Only the first client can be impaired (loopback_harness.h); the second,
 *   added with loopbackHarnessAddClient, is always clean. So the lagged
 *   player is always client 1: the shooter in the tank-shell runs, the man's
 *   owner in the pill run.
 *
 * ── What the script drives, and what it does not ──────────────────────────
 *
 *   - Every tank key, every shot and every build order goes through the client
 *     input path (clientSimNetSendInput), never into the server sim. The man
 *     walks because his owner's client ordered a road.
 *   - The shooter aims with what its client shows of the enemy man
 *     (interpGetLgm on the client's own InterpContext — the same entry the
 *     renderer draws him from).
 *   - Pumps are paced to one 20ms frame of wall clock each
 *     (loopbackHarnessSetPumpInterval), for #343's reason: the delay the
 *     impairment layer holds a datagram for and the delay the server reads off
 *     its ping measurement are then the same number of frames. One pump is
 *     exactly one serverSimTick, so one pump is one history entry.
 *   - Fixture, all of it the test's own doing: the carved grass arena, the
 *     tanks' parking places (moved once after a calibration shot, before the
 *     man sets out), their stock topped up every pump. A parked tank is taken
 *     off its spawn boat (a boat shell goes off on the first land square) and
 *     is put back if a shell shoves it (see lkr_place). In the pill run the
 *     pillbox is also held silent (its reload counter set to 0 each pump) and
 *     let off at chosen frames — the pill is the thing firing, not the thing
 *     under test, and holding it is the only way to time its shells against
 *     the man.
 *   - Every run first fires one calibration shell to measure where a shell
 *     from that spot goes off and how many frames it flies, so nothing here
 *     depends on the shell range rules.
 *
 * What was measured is printed: pings, the shell's rewind, the client's view
 * of the man beside the server's, the explosion point, and the man's current
 * and rewound positions with their distances to it. MAP_SQUARE_MIDDLE is the
 * kill radius.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim.sim / tick / playerPing / lgmPosHistory */
#include "game_sim.h"
#include "client_sim.h"            /* clientSimGetInterpCtx */
#include "client_net.h"            /* clientSimGetViewTick */
#include "client_connect_state.h"
#include "input_packet.h"
#include "interpolation.h"         /* interpGetLgm — the client's view of a man */
#include "position_history.h"      /* posHistoryGet — the server's rewind ring */
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "lgm.h"
#include "shells.h"
#include "threads.h"
#include "log.h"
#include "brain_record.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Same output dirs as test_lgm_under_fire.c; CMake sets both. */
#ifndef WB_TEST_REPLAY_DIR
#define WB_TEST_REPLAY_DIR "replays"
#endif
#ifndef WB_TEST_DEBUG_SESSION_DIR
#define WB_TEST_DEBUG_SESSION_DIR "debug_sessions"
#endif

/* ── arena ──────────────────────────────────────────────────────────────── */

#define LKR_ARENA_W      30   /* carved grass, in map squares               */
#define LKR_FIGHT_W      12   /* columns east of the start the fight uses   */

/* The Log Viewer's fixed opening box, as in test_lgm_under_fire.c. */
#define LKR_VIEW_X0     127
#define LKR_VIEW_SIZE    30
#define LKR_VIEW_MID    (LKR_VIEW_X0 + LKR_VIEW_SIZE / 2)

/* Tank-shell runs: the shooter sits on the start square facing east, the
 * owner's tank LKR_OWNER_ROWS squares north of the line of fire, and the man
 * is sent the same distance south of it, so he walks straight across it. */
#define LKR_SHOT_UP       6
#define LKR_SHOT_DOWN     6
#define LKR_OWNER_ROWS    5

/* Pill run: pill LKR_PILL_COL squares east of the start, the target tank
 * LKR_TARGET_GAP squares west of the pill, and the man's own tank far enough
 * north that the pill never picks it (pill_range is 8 squares). */
#define LKR_PILL_UP      10
#define LKR_PILL_DOWN     5
#define LKR_PILL_COL     10
#define LKR_TARGET_GAP    3
#define LKR_MAN_NORTH     9
#define LKR_MAN_SOUTH     4

/* Pumps. One pump is one 20ms server frame, so these are frame budgets. */
#define LKR_CONNECT_MAX 4000  /* join + map download, lagged                */
#define LKR_SETTLE      100   /* parked: ping measured, clients synced      */
#define LKR_RESETTLE     50   /* after the post-calibration move            */
#define LKR_SHELL_MAX   150   /* one shell's whole flight, with slack       */
#define LKR_WATCH_MAX   400   /* the man's walk, with slack                 */
#define LKR_PUMP_MS      20

#define LKR_TRAIL_MAX   800
#define LKR_SHELLS_MAX    8

#define LKR_MAN_SPEED   MAP_MANSPEED_TGRASS   /* world units a frame on grass */
#define LKR_KILL        MAP_SQUARE_MIDDLE     /* open-ground kill radius      */

/* How late the bounded run fires, in frames past the right moment. The man
 * then stands about this many frames of walk (12 x 16 = 192 world units)
 * beyond the explosion even at his rewound position. */
#define LKR_BOUNDED_LATE 12

/* Pill run: how far clear of the kill radius both the "is" and the "was"
 * positions must be for a shell to be let off — one frame of walk. */
#define LKR_PILL_MARGIN  16

typedef struct {
    int  up, down;            /* rows carved above and below the start row */
    bool withPill;
    BYTE startNum;            /* 1-based                                   */
    BYTE startX, startY;
    BYTE pillX, pillY;
    bool overBases;
    bool ok;
} LkrArena;

/* ── arena construction (runs before the clients join) ───────────────────── */

static bool lkr_rect_base_free(GameSim *gs, int x0, int y0, int h) {
    int i, j;
    for (j = -1; j <= h; j++) {
        for (i = -1; i <= LKR_ARENA_W; i++) {
            if (basesExistPos(&gs->bs, (BYTE)(x0 + i), (BYTE)(y0 + j))) {
                return false;
            }
        }
    }
    return true;
}

static bool lkr_rect_fits(int x0, int y0, int h) {
    return x0 > 1 && y0 > 1 &&
           x0 + LKR_ARENA_W + 1 < MAP_ARRAY_SIZE &&
           y0 + h + 1 < MAP_ARRAY_SIZE;
}

static bool lkr_on_screen(int startX, int startY, int up, int down) {
    int lo = LKR_VIEW_X0 + 1;
    int hi = LKR_VIEW_X0 + LKR_VIEW_SIZE - 1;
    return startX >= lo && startX + LKR_FIGHT_W <= hi &&
           startY - up >= lo && startY + down <= hi;
}

/* test_lgm_under_fire.c's start picker, with the strip's height a parameter:
 * a base-free strip first, then a fight inside the Log Viewer's box, then the
 * start nearest the box's middle. Returns the 1-based start or 0. */
static BYTE lkr_pick_start(GameSim *gs, const LkrArena *a, BYTE *outX,
                           BYTE *outY, bool *overBases) {
    BYTE n = startsGetNumStarts(&gs->ss);
    BYTE best = 0, bestX = 0, bestY = 0;
    bool bestOverBases = false;
    long bestScore = 0;
    int  h = a->up + a->down + 1;
    BYTE i;

    for (i = 1; i <= n; i++) {
        start s;
        int  x0, y0, dx, dy;
        bool crossesBase;
        long score;

        memset(&s, 0, sizeof(s));
        startsGetStartStruct(&gs->ss, &s, i);
        x0 = (int)s.x;
        y0 = (int)s.y - a->up;
        if (!lkr_rect_fits(x0, y0, h)) {
            continue;
        }
        crossesBase = !lkr_rect_base_free(gs, x0, y0, h);
        dx = (int)s.x - LKR_VIEW_MID;
        dy = (int)s.y - LKR_VIEW_MID;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        score = (crossesBase ? 1000000L : 0L)
              + (lkr_on_screen((int)s.x, (int)s.y, a->up, a->down) ? 0L : 1000L)
              + (long)(dx > dy ? dx : dy);
        if (best == 0 || score < bestScore) {
            best          = i;
            bestX         = s.x;
            bestY         = s.y;
            bestOverBases = crossesBase;
            bestScore     = score;
        }
    }
    if (best != 0) {
        *outX = bestX;
        *outY = bestY;
        *overBases = bestOverBases;
    }
    return best;
}

/* Carve the grass, send every slot to the chosen start, and leave either no
 * pillbox at all (tank-shell runs: nothing but the shooter may fire) or one
 * neutral one (the pill run). */
static void lkr_build_arena(LoopbackHarness *h, void *user) {
    LkrArena *a = (LkrArena *)user;
    GameSim  *gs = &h->sim->sim;
    int x, y, i;

    a->ok = false;
    a->startNum = lkr_pick_start(gs, a, &a->startX, &a->startY, &a->overBases);
    if (a->startNum == 0) {
        return;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        gs->scenarioStartIdx[i] = (BYTE)(a->startNum - 1);
    }
    for (y = -a->up; y <= a->down; y++) {
        for (x = 0; x < LKR_ARENA_W; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)(a->startX + x), (BYTE)(a->startY + y),
                      GRASS, FALSE, TRUE);
        }
    }

    if (a->withPill) {
        pillbox p;
        a->pillX = (BYTE)(a->startX + LKR_PILL_COL);
        a->pillY = a->startY;
        memset(&p, 0, sizeof(p));
        p.x        = a->pillX;
        p.y        = a->pillY;
        p.owner    = NEUTRAL;
        p.armour   = (BYTE)gs->rules.pill_max_armour;
        p.speed    = (BYTE)gs->rules.pill_attack_ticks;
        p.inTank   = FALSE;
        p.reload   = 0;
        p.coolDown = 0;
        p.justSeen = FALSE;
        gs->pb->item[0]   = p;
        gs->pb->active[0] = 1;
        gs->pb->numPills  = 1;
    } else {
        gs->pb->numPills = 0;
    }
    a->ok = true;
}

/* ── small readers ──────────────────────────────────────────────────────── */

static WORLD lkr_world(BYTE mapPos) {
    return (WORLD)(((int)mapPos << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

static double lkr_dist(int ax, int ay, int bx, int by) {
    double dx = (double)(ax - bx);
    double dy = (double)(ay - by);
    return sqrt(dx * dx + dy * dy);
}

static bool lkr_connected(LoopbackHarness *h, void *user) {
    (void)user;
    brainRecordTick(h->sim);
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool lkr_connected2(LoopbackHarness *h, void *user) {
    (void)user;
    brainRecordTick(h->sim);
    return clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* ── the run's state and its one step ────────────────────────────────────── */

/* A one-shot riding one frame's input: fire, a build order, or neither. */
typedef struct {
    uint8_t actions;
    uint8_t build;            /* InputPacket.buildAction, 1-based, 0 = none */
    BYTE    bx, by;
} LkrOrder;

typedef struct {
    LoopbackHarness h;
    GameSim  *gs;
    BYTE      s1, s2;         /* client 1 (maybe lagged), client 2 (clean) */
    uint32_t  tick1, tick2;   /* each client's own input tick counter      */
    bool      holdPill;       /* pill run: keep the pill's reload at 0     */
    bool      releasePill;    /* let it fire in the next step              */
    int       pump;           /* steps since the paced part began          */
    bool      parked[MAX_TANKS];          /* held at parkX/parkY each step */
    WORLD     parkX[MAX_TANKS], parkY[MAX_TANKS];
} LkrRun;

/* test_lgm_under_fire.c's input rate: three input ticks a pump, one-shots on
 * the last even one — see luf_send_frame there for why three. */
#define LKR_INPUTS_PER_PUMP 3

static void lkr_send_frame(struct ClientSim *cs, BYTE slot, uint32_t *tick,
                           const LkrOrder *o) {
    uint32_t base = *tick;
    uint32_t gameTick = 0;
    int n;

    for (n = 1; n <= LKR_INPUTS_PER_PUMP; n++) {
        if (((base + (uint32_t)n) % 2u) == 0u) {
            gameTick = base + (uint32_t)n;
        }
    }
    for (n = 1; n <= LKR_INPUTS_PER_PUMP; n++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = base + (uint32_t)n;
        pkt.playerNum = slot;
        if (o != NULL && pkt.tick == gameTick) {
            pkt.actions     = o->actions;
            pkt.buildAction = o->build;
            pkt.buildX      = o->bx;
            pkt.buildY      = o->by;
        }
        clientSimNetSendInput(cs, &pkt);
    }
    *tick = base + (uint32_t)LKR_INPUTS_PER_PUMP;
}

static void lkr_restock(LkrRun *r, BYTE slot) {
    GameSim *gs = r->gs;
    threadsWaitForMutex();
    if (gs->tanks[slot] != NULL) {
        tankSetArmour(&gs->tanks[slot], (BYTE)gs->rules.tank_full_armour);
        tankSetShells(gs, &gs->tanks[slot], (BYTE)gs->rules.tank_full_shells);
        tankSetTrees(gs, &gs->tanks[slot], (BYTE)gs->rules.tank_full_trees);
    }
    threadsReleaseMutex();
}

/* Park a tank. Map starts are in the sea, so a tank that spawned there and
 * never drove is still on its boat even with grass carved under it — and a
 * shell fired from a boat goes off on the first land square it meets
 * (shellsCalcCollision's mapIsPassable test uses the shell's onBoat). The
 * tank is taken off the boat so its shells fly across the grass.
 *
 * A parked tank stays parked: a shell that hits a tank and does not kill it
 * shoves it along the shell's line over the next frames (bumpX/bumpY, set in
 * tankIsTankHit). The pill run's target tank takes several pill shells, and
 * each shove would move the point the next shell goes off. lkr_step puts a
 * shoved tank back and clears the shove before every frame. */
static void lkr_park_locked(LkrRun *r, BYTE slot) {
    tank *t = &r->gs->tanks[slot];
    tankSetWorld(r->gs, t, r->parkX[slot], r->parkY[slot],
                 (TURNTYPE)BRADIANS_EAST, false);
    tankSetOnBoat(t, FALSE);
    (*t)->speed = 0;
    (*t)->bumpX = 0;
    (*t)->bumpY = 0;
}

static void lkr_place(LkrRun *r, BYTE slot, WORLD x, WORLD y) {
    r->parked[slot] = true;
    r->parkX[slot]  = x;
    r->parkY[slot]  = y;
    threadsWaitForMutex();
    lkr_park_locked(r, slot);
    threadsReleaseMutex();
}

static void lkr_hold_parked(LkrRun *r) {
    BYTE slot;
    threadsWaitForMutex();
    for (slot = 0; slot < MAX_TANKS; slot++) {
        tank *t = &r->gs->tanks[slot];
        if (!r->parked[slot] || *t == NULL) continue;
        if ((*t)->x != r->parkX[slot] || (*t)->y != r->parkY[slot] ||
            (*t)->bumpX != 0 || (*t)->bumpY != 0) {
            lkr_park_locked(r, slot);
        }
    }
    threadsReleaseMutex();
}

/* One frame: the pill's hold or release, both clients' input (tanks parked,
 * throttle off), one pump, the session frame, and the stock top-up. Every
 * pump of the paced part goes through here. */
static void lkr_step(LkrRun *r, const LkrOrder *o1, const LkrOrder *o2) {
    if (r->gs->pb->numPills > 0 && (r->holdPill || r->releasePill)) {
        pillbox *p = &r->gs->pb->item[0];
        threadsWaitForMutex();
        if (r->releasePill) {
            /* Due now, and already looking at its target, so pillsUpdate
             * fires in this very frame rather than spending one "seeing". */
            p->reload   = p->speed;
            p->justSeen = TRUE;
        } else {
            p->reload = 0;
        }
        threadsReleaseMutex();
        r->releasePill = false;
    }
    lkr_hold_parked(r);
    lkr_send_frame(r->h.cs, r->s1, &r->tick1, o1);
    lkr_send_frame(r->h.cs2, r->s2, &r->tick2, o2);
    loopbackHarnessPump(&r->h);
    brainRecordTick(r->h.sim);
    lkr_restock(r, r->s1);
    lkr_restock(r, r->s2);
    r->pump++;
}

/* ── following shells on the server ─────────────────────────────────────── */

/* A shell that has gone into a snapshot is deleted the instant it explodes
 * (serverSimTick sets packSent on every shell, server_sim_tick.c:916), so the
 * server never shows a shell in its "dead" state. What it does show is the
 * shell every frame until then. Where it went off follows from the last look:
 *   - it ran out of range (length was SHELL_DEATH): it went off where it was;
 *   - otherwise it hit something, one step further on — the step shellsUpdate
 *     takes with shellAdvance1Tick before testing for a collision.
 * The global shell hit log is not used: the clients in this process run
 * shellsUpdate too and write into it. */
typedef struct {
    struct shellsObj *ptr;    /* identity while in flight                  */
    bool     landed;
    bool     ranOut;          /* false = hit something                     */
    int      firstPump, landPump;
    uint8_t  comp;            /* compensationTicks the shell carries       */
    BYTE     lastLength;
    WORLD    x, y;            /* last seen, then where it went off         */
    WORLD    firstX, firstY;  /* first seen                                */
    int32_t  xAcc, yAcc, xStep, yStep;
} LkrShell;

typedef struct {
    BYTE     owner;
    int      count;
    LkrShell s[LKR_SHELLS_MAX];
} LkrShellWatch;

static void lkr_watch_arm(LkrShellWatch *w, BYTE owner) {
    memset(w, 0, sizeof(*w));
    w->owner = owner;
}

static struct shellsObj *lkr_find_shell(GameSim *gs, struct shellsObj *want,
                                        BYTE owner) {
    shells q = gs->shs;
    while (q != NULL) {
        if (q == want && q->owner == owner && !q->shellDead) {
            return q;
        }
        q = q->next;
    }
    return NULL;
}

/* Call after every step. Picks up new shells of the watched owner, refreshes
 * the ones still flying, and closes the ones that have gone. */
static void lkr_watch(LkrRun *r, LkrShellWatch *w) {
    shells q;
    int i;

    for (i = 0; i < w->count; i++) {
        LkrShell *s = &w->s[i];
        struct shellsObj *live;
        if (s->landed) continue;
        live = lkr_find_shell(r->gs, s->ptr, w->owner);
        /* A freed shell's memory can come back as a new shell; a length that
         * went UP is that, and the old one has gone. */
        if (live != NULL && live->length <= s->lastLength) {
            s->x = live->x;  s->y = live->y;
            s->xAcc = live->xAcc;  s->yAcc = live->yAcc;
            s->lastLength = live->length;
            continue;
        }
        s->landed   = true;
        s->landPump = r->pump;
        s->ranOut   = (s->lastLength <= SHELL_DEATH);
        if (!s->ranOut) {
            shellAdvance1Tick(&s->x, &s->y, &s->xAcc, &s->yAcc, s->xStep,
                              s->yStep);
        }
    }
    for (q = r->gs->shs; q != NULL; q = q->next) {
        bool known = false;
        if (q->owner != w->owner || q->shellDead) continue;
        for (i = 0; i < w->count; i++) {
            if (!w->s[i].landed && w->s[i].ptr == q) {
                known = true;
                break;
            }
        }
        if (known || w->count >= LKR_SHELLS_MAX) continue;
        {
            LkrShell *s = &w->s[w->count++];
            memset(s, 0, sizeof(*s));
            s->ptr        = q;
            s->firstPump  = r->pump;
            s->comp       = q->compensationTicks;
            s->lastLength = q->length;
            s->x = q->x;  s->y = q->y;
            s->firstX = q->x;  s->firstY = q->y;
            s->xAcc = q->xAcc;  s->yAcc = q->yAcc;
            s->xStep = q->xStep;  s->yStep = q->yStep;
        }
    }
}

/* ── the man, as the server tested him ──────────────────────────────────── */

typedef struct {
    bool   haveRew, haveCur;
    int    rewX, rewY, curX, curY;
    double rewDist, curDist;
    uint8_t rewind;
} LkrManAt;

/* Read in the step the shell went off: the ring has not been written since
 * shellsUpdate ran, so entry 0 is where the man stood in that frame and entry
 * `rewind` is the position shellsUpdate tested. */
static void lkr_man_at(LkrRun *r, BYTE owner, uint8_t rewind, int ix, int iy,
                       LkrManAt *m) {
    const PosHistory *ph = &r->h.sim->lgmPosHistory[owner];
    WORLD x = 0, y = 0;
    memset(m, 0, sizeof(*m));
    m->rewind = rewind;
    if (posHistoryGet(ph, 0, &x, &y)) {
        m->haveCur = true;
        m->curX = (int)x;  m->curY = (int)y;
        m->curDist = lkr_dist(ix, iy, m->curX, m->curY);
    }
    if (rewind > 0 && posHistoryGet(ph, rewind, &x, &y)) {
        m->haveRew = true;
        m->rewX = (int)x;  m->rewY = (int)y;
        m->rewDist = lkr_dist(ix, iy, m->rewX, m->rewY);
    }
}

static void lkr_print_man_at(const char *what, const LkrManAt *m, int ix,
                             int iy) {
    fprintf(stderr, "    %s: explosion at world (%d,%d)\n", what, ix, iy);
    if (m->haveCur) {
        fprintf(stderr, "      man NOW          (%5d,%5d)  distance %6.1f  %s\n",
                m->curX, m->curY, m->curDist,
                (m->curDist <= LKR_KILL) ? "<= kill radius" : "clear");
    } else {
        fprintf(stderr, "      man NOW          (none: dead or in his tank)\n");
    }
    if (m->haveRew) {
        fprintf(stderr, "      man %2u frames ago (%5d,%5d)  distance %6.1f  %s\n",
                (unsigned)m->rewind, m->rewX, m->rewY, m->rewDist,
                (m->rewDist <= LKR_KILL) ? "<= kill radius" : "clear");
    } else {
        fprintf(stderr, "      man %2u frames ago (none in the ring)\n",
                (unsigned)m->rewind);
    }
}

/* Per-frame record of the man on the server. */
typedef struct {
    int      pump;
    uint32_t tick;            /* server tick after the pump (half-steps)    */
    int      x, y;
    bool     out, dead;
} LkrTrail;

static void lkr_trail_add(LkrRun *r, BYTE owner, LkrTrail *t, int *n) {
    lgm *man = &r->gs->lgmen[owner];
    if (*n >= LKR_TRAIL_MAX || *man == NULL) return;
    t[*n].pump = r->pump;
    t[*n].tick = r->h.sim->tick;
    t[*n].x    = (int)(*man)->x;
    t[*n].y    = (int)(*man)->y;
    t[*n].out  = !(*man)->inTank;
    t[*n].dead = (*man)->isDead;
    (*n)++;
}

/* ── recording, as test_lgm_under_fire.c does it ────────────────────────── */

static void lkr_session_start(const char *tag, char *dirOut, size_t cap) {
    time_t    now = time(NULL);
    struct tm tmv;
    char      stamp[32];

    dirOut[0] = '\0';
#ifdef _WIN32
    if (localtime_s(&tmv, &now) != 0) {
        return;
    }
#else
    if (localtime_r(&now, &tmv) == NULL) {
        return;
    }
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    snprintf(dirOut, cap, "%s/%s_%s", WB_TEST_DEBUG_SESSION_DIR, tag, stamp);
    SDL_CreateDirectory(WB_TEST_DEBUG_SESSION_DIR);
    if (!SDL_CreateDirectory(dirOut)) {
        fprintf(stderr, "  session: COULD NOT create %s (%s) — the run goes "
                        "on unrecorded\n", dirOut, SDL_GetError());
        dirOut[0] = '\0';
        return;
    }
    brainRecordSetSessionDir(dirOut);
    brainRecordSetEnabled(true);
    fprintf(stderr, "  session: recording to %s\n", dirOut);
}

static void lkr_session_stop(const char *dir) {
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
    if (dir != NULL && dir[0] != '\0') {
        fprintf(stderr, "  session written: %s  (BrainTest -> Load Session)\n",
                dir);
    }
}

static void lkr_record_start(LoopbackHarness *h, const char *leaf,
                             char *pathOut, size_t pathCap) {
    snprintf(pathOut, pathCap, "%s/%s", WB_TEST_REPLAY_DIR, leaf);
    SDL_CreateDirectory(WB_TEST_REPLAY_DIR);
    logSetLobbyMode(FALSE);
    if (logStart(pathOut, h->sim, 0, MAX_TANKS, FALSE) != TRUE) {
        fprintf(stderr, "  replay: COULD NOT open %s — the run goes on "
                        "unrecorded\n", pathOut);
        pathOut[0] = '\0';
        return;
    }
    fprintf(stderr, "  replay: recording to %s\n", pathOut);
}

static void lkr_record_stop(const char *path) {
    logStop();
    if (path != NULL && path[0] != '\0') {
        fprintf(stderr, "  replay written: %s\n", path);
    }
}

#define LKR_STOP()                                                          \
    do {                                                                    \
        lkr_record_stop(replayPath);                                        \
        lkr_session_stop(sessionDir);                                        \
        loopbackHarnessStop(&r.h);                                          \
    } while (0)

/* Bring up server + client 1 (cfg impairment) + client 2 (clean), paced from
 * then on, with the arena built and recording started. Returns NULL on
 * success or a failure message; the caller stops the harness either way. */
static const char *lkr_bring_up(LkrRun *r, LkrArena *arena, const char *impair,
                                uint64_t seed, const char *replayLeaf,
                                const char *sessionTag, char *replayPath,
                                size_t replayCap, char *sessionDir,
                                size_t sessionCap) {
    if (!loopbackHarnessStartPrepared(&r->h, "Client1", /*lobbyMode*/ false,
                                      impair, seed, lkr_build_arena, arena)) {
        return "harness start failed";
    }
    if (!arena->ok) {
        return "no start on the map has room for the arena";
    }
    lkr_record_start(&r->h, replayLeaf, replayPath, replayCap);
    lkr_session_start(sessionTag, sessionDir, sessionCap);
    if (loopbackHarnessPumpUntil(&r->h, LKR_CONNECT_MAX, lkr_connected,
                                 NULL) < 0) {
        return "client 1 never connected";
    }
    if (!loopbackHarnessAddClient(&r->h, "Client2")) {
        return "client 2 could not start connecting";
    }
    if (loopbackHarnessPumpUntil(&r->h, LKR_CONNECT_MAX, lkr_connected2,
                                 NULL) < 0) {
        return "client 2 never connected";
    }
    loopbackHarnessSetPumpInterval(&r->h, LKR_PUMP_MS);

    r->gs = &r->h.sim->sim;
    r->s1 = clientSimGetMyPlayerNum(r->h.cs);
    r->s2 = clientSimGetMyPlayerNum(r->h.cs2);
    if (r->s1 >= MAX_TANKS || r->s2 >= MAX_TANKS || r->s1 == r->s2 ||
        r->gs->tanks[r->s1] == NULL || r->gs->tanks[r->s2] == NULL ||
        r->gs->lgmen[r->s1] == NULL || r->gs->lgmen[r->s2] == NULL) {
        return "the two clients did not get two tanks";
    }
    fprintf(stderr, "  arena: map start %u at square (%u,%u)%s; client 1 is "
                    "slot %u, client 2 is slot %u\n",
            (unsigned)arena->startNum, (unsigned)arena->startX,
            (unsigned)arena->startY,
            arena->overBases ? " (the strip crosses a base)" : "",
            (unsigned)r->s1, (unsigned)r->s2);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * Tank-shell runs
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *label;
    const char *impair;       /* client 1 = the shooter                    */
    const char *replayLeaf;
    const char *sessionTag;
    uint64_t    seed;
    int         lateFrames;   /* 0 = aim at the client's view of the man   */
    bool        expectKill;
    bool        requireRewindDecides; /* current position alone must miss */
} LkrShotConfig;

static int lkr_shot_run(const LkrShotConfig *cfg) {
    LkrRun        r;
    LkrArena      arena;
    LkrShellWatch watch;
    LkrTrail      trail[LKR_TRAIL_MAX];
    int           trailCount = 0;
    char          replayPath[512];
    char          sessionDir[512];
    const char   *err;
    lgm          *man;
    int           i;

    /* calibration */
    WORLD t1x = 0, t1y = 0;
    int   rangeX = 0, rangeY = 0, flight = 0;
    uint8_t calComp = 0;
    /* the aim point: where a shell from the parked shooter goes off */
    int   aimX = 0, aimY = 0;
    BYTE  col;
    WORLD t2x, t2y;
    BYTE  destX, destY;
    /* the shot */
    bool     fired = false;
    int      firePump = 0;
    int      viewX = 0, viewY = 0;
    uint32_t viewTick = 0, fireSrvTick = 0;
    int      fireSrvManX = 0, fireSrvManY = 0;
    uint16_t pingShooter = 0, pingOwner = 0;
    bool     manDied = false;
    double   passClosest = 1e9;   /* bounded run: closest the man came  */
    int      lastInFrames = -1;   /* ... and when he was last inside    */
    LkrShell shot;
    LkrManAt at;

    memset(&r, 0, sizeof(r));
    memset(&arena, 0, sizeof(arena));
    memset(&shot, 0, sizeof(shot));
    memset(&at, 0, sizeof(at));
    replayPath[0] = '\0';
    sessionDir[0] = '\0';
    arena.up       = LKR_SHOT_UP;
    arena.down     = LKR_SHOT_DOWN;
    arena.withPill = false;

    fprintf(stderr, "\n  ==== %s (shooter impair=%s, owner clean, seed=%llu) "
                    "====\n", cfg->label,
            (cfg->impair != NULL) ? cfg->impair : "clean",
            (unsigned long long)cfg->seed);

    err = lkr_bring_up(&r, &arena, cfg->impair, cfg->seed, cfg->replayLeaf,
                       cfg->sessionTag, replayPath, sizeof(replayPath),
                       sessionDir, sizeof(sessionDir));
    if (err != NULL) {
        LKR_STOP();
        UT_FAIL("%s: %s", cfg->label, err);
    }
    man = &r.gs->lgmen[r.s2];

    /* Park both. The shooter on the start square facing east; the owner's
     * tank well off the line of fire for the calibration shot. */
    t1x = lkr_world(arena.startX);
    t1y = lkr_world(arena.startY);
    lkr_place(&r, r.s1, t1x, t1y);
    lkr_place(&r, r.s2, lkr_world((BYTE)(arena.startX + 2)),
              lkr_world((BYTE)(arena.startY - LKR_OWNER_ROWS)));
    for (i = 0; i < LKR_SETTLE; i++) {
        lkr_step(&r, NULL, NULL);
    }

    /* Calibration: one shell from the parked shooter. */
    {
        LkrOrder fire;
        memset(&fire, 0, sizeof(fire));
        fire.actions = INPUT_ACTION_FIRE;
        lkr_watch_arm(&watch, r.s1);
        lkr_step(&r, &fire, NULL);
        lkr_watch(&r, &watch);
        for (i = 0; i < LKR_SHELL_MAX; i++) {
            if (watch.count > 0 && watch.s[0].landed) break;
            lkr_step(&r, NULL, NULL);
            lkr_watch(&r, &watch);
        }
        if (watch.count == 0 || !watch.s[0].landed) {
            LKR_STOP();
            UT_FAIL("%s: the calibration shell never %s", cfg->label,
                    watch.count == 0 ? "appeared" : "went off");
        }
        if (!watch.s[0].ranOut) {
            LKR_STOP();
            UT_FAIL("%s: the calibration shell hit something at (%d,%d) — "
                    "the arena is not clear", cfg->label, (int)watch.s[0].x,
                    (int)watch.s[0].y);
        }
        rangeX  = (int)watch.s[0].x - (int)t1x;
        rangeY  = (int)watch.s[0].y - (int)t1y;
        flight  = watch.s[0].landPump - watch.s[0].firstPump;
        calComp = watch.s[0].comp;
    }

    /* Move the shooter by less than a square so its shells go off exactly on
     * a square's centre line, and put the owner's tank on that line: his man
     * then walks due south along x = aimX and crosses the line of fire at the
     * very point shells from here go off. */
    col  = (BYTE)(((int)t1x + rangeX) >> TANK_SHIFT_MAPSIZE);
    aimX = (int)lkr_world(col);
    t1x  = (WORLD)(aimX - rangeX);
    aimY = (int)t1y + rangeY;
    t2x  = (WORLD)aimX;
    t2y  = (WORLD)(lkr_world(arena.startY) - LKR_OWNER_ROWS * 256);
    destX = col;
    destY = (BYTE)(arena.startY + LKR_OWNER_ROWS);
    lkr_place(&r, r.s1, t1x, t1y);
    lkr_place(&r, r.s2, t2x, t2y);
    for (i = 0; i < LKR_RESETTLE; i++) {
        lkr_step(&r, NULL, NULL);
    }

    pingShooter = r.h.sim->playerPing[r.s1];
    pingOwner   = r.h.sim->playerPing[r.s2];
    fprintf(stderr,
            "  calibration: a shell from the shooter goes off %d,%d world "
            "units away after %d frames (rewind %u)\n"
            "  shooter at world (%u,%u) facing east; aim point (%d,%d); owner "
            "tank (%u,%u); man ordered to square (%u,%u)\n"
            "  pings: shooter %ums, owner %ums\n",
            rangeX, rangeY, flight, (unsigned)calComp, (unsigned)t1x,
            (unsigned)t1y, aimX, aimY, (unsigned)t2x, (unsigned)t2y,
            (unsigned)destX, (unsigned)destY, (unsigned)pingShooter,
            (unsigned)pingOwner);

    /* Send the man. His owner's client orders a road on the far square. */
    {
        LkrOrder road;
        memset(&road, 0, sizeof(road));
        road.build = (uint8_t)(LGM_ROAD_REQUEST + 1);
        road.bx    = destX;
        road.by    = destY;
        lkr_step(&r, NULL, &road);
        lkr_trail_add(&r, r.s2, trail, &trailCount);
    }

    /* Watch him walk. The shooter's rule is a player's: fire when the man its
     * client shows, walked on for the shell's flight, reaches the aim point.
     * The bounded run fires lateFrames later than that. */
    lkr_watch_arm(&watch, r.s1);
    for (i = 0; i < LKR_WATCH_MAX; i++) {
        LkrOrder fire;
        bool fireNow = false;

        if (!fired) {
            BYTE mx = 0, my = 0, px = 0, py = 0, fr = 0;
            if (interpGetLgm(clientSimGetInterpCtx(r.h.cs), r.s2, &mx, &my,
                             &px, &py, &fr)) {
                int vx = ((int)mx << TANK_SHIFT_MAPSIZE) + ((int)px << 4);
                int vy = ((int)my << TANK_SHIFT_MAPSIZE) + ((int)py << 4);
                if (vy >= (int)t2y + 64 &&
                    vy + LKR_MAN_SPEED * (flight - cfg->lateFrames) >= aimY) {
                    fireNow     = true;
                    viewX       = vx;
                    viewY       = vy;
                    viewTick    = clientSimGetViewTick(r.h.cs);
                    fireSrvTick = r.h.sim->tick;
                    fireSrvManX = (int)(*man)->x;
                    fireSrvManY = (int)(*man)->y;
                }
            }
        }
        memset(&fire, 0, sizeof(fire));
        fire.actions = INPUT_ACTION_FIRE;
        lkr_step(&r, fireNow ? &fire : NULL, NULL);
        if (fireNow) {
            fired    = true;
            firePump = r.pump;
        }
        lkr_trail_add(&r, r.s2, trail, &trailCount);
        lkr_watch(&r, &watch);

        if (watch.count > 0 && watch.s[0].landed) {
            shot = watch.s[0];
            lkr_man_at(&r, r.s2, shot.comp, (int)shot.x, (int)shot.y, &at);
            manDied = (*man)->isDead;
            break;
        }
        if ((*man)->isDead) {
            manDied = true;
            break;   /* something else killed him: reported below */
        }
    }
    pingShooter = r.h.sim->playerPing[r.s1];

    /* ── the printout ───────────────────────────────────────────────────── */
    if (fired) {
        /* The client's copy of the man is quantized to 16 world units
         * (client_snapshot.c packs map square + 16-unit pixel), so match the
         * server frame whose position rounds to what the client shows. */
        int k, seenAt = -1;
        for (k = 0; k < trailCount; k++) {
            if (abs(trail[k].x - viewX) <= 16 && (trail[k].y & ~15) == viewY &&
                trail[k].tick <= fireSrvTick) {
                seenAt = k;
            }
        }
        fprintf(stderr,
                "  fire sent on pump %d, decided at server tick %u\n"
                "    client 1 shows the man at (%d,%d); its viewTick is %u "
                "(%u frames old)\n"
                "    the server has him at     (%d,%d)\n",
                firePump, (unsigned)fireSrvTick, viewX, viewY,
                (unsigned)viewTick,
                (unsigned)((fireSrvTick - viewTick) / 2), fireSrvManX,
                fireSrvManY);
        if (seenAt >= 0) {
            fprintf(stderr, "    the server had him at the client's position "
                            "after server tick %u: the picture is %u frames "
                            "old\n",
                    (unsigned)trail[seenAt].tick,
                    (unsigned)((fireSrvTick - trail[seenAt].tick) / 2));
        } else {
            fprintf(stderr, "    (no server frame rounds to the client's "
                            "picture)\n");
        }
        if (shot.firstPump > 0) {
            fprintf(stderr, "    the fire input reached the server %d frames "
                            "later\n", shot.firstPump - firePump);
        }
    } else {
        fprintf(stderr, "  the shooter never fired: client 1 never showed the "
                        "man at the firing point\n");
    }
    if (shot.landed) {
        fprintf(stderr, "  the shell: created pump %d, went off pump %d "
                        "(%d frames), %s, carrying a rewind of %u frames; "
                        "shooter ping at the end %ums\n",
                shot.firstPump, shot.landPump,
                shot.landPump - shot.firstPump,
                shot.ranOut ? "ran out of range" : "HIT SOMETHING",
                (unsigned)shot.comp, (unsigned)pingShooter);
        lkr_print_man_at("the shot", &at, (int)shot.x, (int)shot.y);
        if (cfg->lateFrames > 0) {
            /* When the man was last inside the kill radius of this explosion
             * point: the shot went where he WAS, that many frames back. */
            int k;
            for (k = 0; k < trailCount; k++) {
                double d;
                if (!trail[k].out || trail[k].dead) continue;
                d = lkr_dist((int)shot.x, (int)shot.y, trail[k].x, trail[k].y);
                if (d < passClosest) {
                    passClosest = d;
                }
                if (d <= LKR_KILL && trail[k].pump <= shot.landPump) {
                    lastInFrames = shot.landPump - trail[k].pump;
                }
            }
            if (lastInFrames >= 0) {
                fprintf(stderr, "      the man walked through the explosion "
                                "point (closest %.1f); he was last inside the "
                                "kill radius %d frames before it went off — "
                                "outside a rewind of %u\n",
                        passClosest, lastInFrames, (unsigned)shot.comp);
            } else {
                fprintf(stderr, "      the man never came within the kill "
                                "radius of the explosion point (closest "
                                "%.1f)\n", passClosest);
            }
        }
    }
    fprintf(stderr, "    outcome: %s\n", manDied ? "THE MAN DIED" : "the man lived");

    LKR_STOP();

    UT_ASSERT_MSG(fired, "%s: the shooter never fired — client 1 never showed "
                         "the man walking to the aim point", cfg->label);
    UT_ASSERT_MSG(shot.landed, "%s: %s", cfg->label,
                  manDied ? "the man died before the shot went off"
                          : "the shot never went off");
    UT_ASSERT_MSG(shot.ranOut, "%s: the shot hit something on its way — the "
                               "arena is not clear", cfg->label);
    UT_ASSERT_MSG(shot.comp > 0, "%s: the shot carries no rewind at all, so "
                                 "this run cannot test one", cfg->label);
    UT_ASSERT_MSG(at.haveRew, "%s: the server had no rewound position for the "
                              "man %u frames back", cfg->label,
                  (unsigned)shot.comp);
    /* The engine's own rule, against what it did. */
    UT_ASSERT_MSG(manDied == (at.rewDist <= LKR_KILL),
                  "%s: the man %s, but his position %u frames before the "
                  "explosion was %.1f world units from it (kill radius %d)",
                  cfg->label, manDied ? "died" : "lived", (unsigned)shot.comp,
                  at.rewDist, LKR_KILL);
    if (cfg->requireRewindDecides) {
        UT_ASSERT_MSG(at.haveCur && at.curDist > LKR_KILL,
                      "%s: the man's CURRENT position was %.1f from the "
                      "explosion, inside the kill radius — this run did not "
                      "separate rewind from no rewind", cfg->label, at.curDist);
    }
    if (cfg->expectKill) {
        UT_ASSERT_MSG(manDied, "%s: the shot at where client 1 showed the man "
                               "did not kill him (rewound distance %.1f, "
                               "current %.1f)", cfg->label, at.rewDist,
                      at.curDist);
    } else {
        UT_ASSERT_MSG(lastInFrames > (int)shot.comp,
                      "%s: the late shot did not go where the man was before "
                      "the rewind window (last inside the kill radius %d "
                      "frames before the explosion, rewind %u)", cfg->label,
                      lastInFrames, (unsigned)shot.comp);
        UT_ASSERT_MSG(!manDied, "%s: a shot at where the man was %d+ frames "
                                "earlier killed him — the rewind reached "
                                "further back than %u frames", cfg->label,
                      cfg->lateFrames, (unsigned)shot.comp);
    }
    return 0;
}

int run_lgm_kill_rewind_lagged_shooter(void) {
    LkrShotConfig cfg;
    cfg.label                = "lagged shooter";
    cfg.impair               = "delay=80,jitter=0";
    cfg.replayLeaf           = "lgm_kill_rewind_lagged.wbv";
    cfg.sessionTag           = "lgm_rewind_lagged";
    cfg.seed                 = 4343u;
    cfg.lateFrames           = 0;
    cfg.expectKill           = true;
    cfg.requireRewindDecides = true;
    return lkr_shot_run(&cfg);
}

int run_lgm_kill_rewind_nolag_control(void) {
    LkrShotConfig cfg;
    cfg.label                = "no lag (control)";
    cfg.impair               = NULL;
    cfg.replayLeaf           = "lgm_kill_rewind_nolag.wbv";
    cfg.sessionTag           = "lgm_rewind_nolag";
    cfg.seed                 = 4343u;
    cfg.lateFrames           = 0;
    cfg.expectKill           = true;
    cfg.requireRewindDecides = false;
    return lkr_shot_run(&cfg);
}

int run_lgm_kill_rewind_bounded(void) {
    LkrShotConfig cfg;
    cfg.label                = "bounded (shot fired late)";
    cfg.impair               = "delay=80,jitter=0";
    cfg.replayLeaf           = "lgm_kill_rewind_bounded.wbv";
    cfg.sessionTag           = "lgm_rewind_bounded";
    cfg.seed                 = 4343u;
    cfg.lateFrames           = LKR_BOUNDED_LATE;
    cfg.expectKill           = false;
    cfg.requireRewindDecides = false;
    return lkr_shot_run(&cfg);
}

/* ════════════════════════════════════════════════════════════════════════
 * Pill run
 * ════════════════════════════════════════════════════════════════════════ */

/* One pill shell's story. */
typedef struct {
    const char *name;
    bool     released;
    int      releasePump;
    uint8_t  compAtRelease;
    bool     landed;
    LkrShell shell;
    LkrManAt at;
    bool     diedHere;        /* the man was dead after this shell's step  */
} LkrPillShot;

int run_lgm_kill_rewind_pill_owner_lag(void) {
    LkrRun        r;
    LkrArena      arena;
    LkrShellWatch watch;
    char          replayPath[512];
    char          sessionDir[512];
    const char   *err;
    const char   *label = "pill vs lagged owner's man";
    const char   *impair = "delay=80,jitter=0";
    lgm          *man;
    int           i;
    int           impactX = 0, impactY = 0, flight = 0;
    WORLD         t2x, t2y, t1x, t1y;
    BYTE          col, destX, destY;
    LkrPillShot   shots[2];
    int           next = 0;       /* the next shot to release             */
    int           landedCount = 0;
    uint16_t      pingOwner = 0, pingTarget = 0;
    uint8_t       endComp = 0;

    memset(&r, 0, sizeof(r));
    memset(&arena, 0, sizeof(arena));
    memset(shots, 0, sizeof(shots));
    shots[0].name = "shell 1 (lands where he IS, not where he WAS)";
    shots[1].name = "shell 2 (lands where he WAS, not where he IS)";
    replayPath[0] = '\0';
    sessionDir[0] = '\0';
    arena.up       = LKR_PILL_UP;
    arena.down     = LKR_PILL_DOWN;
    arena.withPill = true;

    fprintf(stderr, "\n  ==== %s (owner impair=%s, target clean) ====\n",
            label, impair);

    err = lkr_bring_up(&r, &arena, impair, 4344u, "lgm_kill_rewind_pill.wbv",
                       "lgm_rewind_pill", replayPath, sizeof(replayPath),
                       sessionDir, sizeof(sessionDir));
    if (err != NULL) {
        LKR_STOP();
        UT_FAIL("%s: %s", label, err);
    }
    man = &r.gs->lgmen[r.s1];
    r.holdPill = true;

    /* Client 2's tank is the pill's target, on the pill's row. Client 1's tank
     * — the man's — waits out of pill range to the north. */
    t2x = lkr_world((BYTE)(arena.pillX - LKR_TARGET_GAP));
    t2y = lkr_world(arena.pillY);
    lkr_place(&r, r.s2, t2x, t2y);
    lkr_place(&r, r.s1, t2x, (WORLD)(t2y - LKR_MAN_NORTH * 256));
    for (i = 0; i < LKR_SETTLE; i++) {
        lkr_step(&r, NULL, NULL);
    }

    /* Calibration, twice: where a pill shell hits the target tank and after
     * how many frames, then again after moving the tank so that point sits on
     * a square's centre line — the line the man will walk down. */
    {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            int relPump;
            lkr_watch_arm(&watch, NEUTRAL);
            r.releasePill = true;
            lkr_step(&r, NULL, NULL);
            relPump = r.pump;
            lkr_watch(&r, &watch);
            for (i = 0; i < LKR_SHELL_MAX; i++) {
                if (watch.count > 0 && watch.s[0].landed) break;
                lkr_step(&r, NULL, NULL);
                lkr_watch(&r, &watch);
            }
            if (watch.count == 0 || !watch.s[0].landed || watch.s[0].ranOut) {
                LKR_STOP();
                UT_FAIL("%s: calibration pass %d — the pill shell %s", label,
                        pass + 1, watch.count == 0 ? "never appeared"
                                  : (!watch.s[0].landed ? "never went off"
                                                        : "missed the tank"));
            }
            impactX = (int)watch.s[0].x;
            impactY = (int)watch.s[0].y;
            flight  = watch.s[0].landPump - relPump;
            fprintf(stderr, "  calibration %d: target tank at (%u,%u); the pill "
                            "shell hit at (%d,%d), %d frames after release\n",
                    pass + 1, (unsigned)t2x, (unsigned)t2y, impactX, impactY,
                    flight);
            fprintf(stderr, "    (shell first seen %d frames after release at "
                            "(%d,%d); target tank on the server at (%d,%d))\n",
                    watch.s[0].firstPump - relPump, (int)watch.s[0].firstX,
                    (int)watch.s[0].firstY, (int)r.gs->tanks[r.s2]->x,
                    (int)r.gs->tanks[r.s2]->y);
            if (pass == 0) {
                int shift = (int)lkr_world((BYTE)(impactX >> TANK_SHIFT_MAPSIZE))
                          - impactX;
                t2x = (WORLD)((int)t2x + shift);
                lkr_place(&r, r.s2, t2x, t2y);
                for (i = 0; i < LKR_RESETTLE; i++) {
                    lkr_step(&r, NULL, NULL);
                }
            }
        }
    }

    /* The man's tank on the impact point's column, due north and out of pill
     * range; the road ordered due south, so he walks straight down across the
     * line of fire in front of the target tank. */
    col   = (BYTE)(impactX >> TANK_SHIFT_MAPSIZE);
    t1x   = lkr_world(col);
    t1y   = (WORLD)(t2y - LKR_MAN_NORTH * 256);
    destX = col;
    destY = (BYTE)(arena.pillY + LKR_MAN_SOUTH);
    lkr_place(&r, r.s1, t1x, t1y);
    for (i = 0; i < LKR_RESETTLE; i++) {
        lkr_step(&r, NULL, NULL);
    }
    pingOwner  = r.h.sim->playerPing[r.s1];
    pingTarget = r.h.sim->playerPing[r.s2];
    fprintf(stderr, "  man's tank at (%u,%u), man ordered to square (%u,%u); "
                    "pings: owner %ums (pill rewind %u), target %ums (pill "
                    "rewind %u)\n",
            (unsigned)t1x, (unsigned)t1y, (unsigned)destX, (unsigned)destY,
            (unsigned)pingOwner, (unsigned)r.gs->perPlayerCompTicks[r.s1],
            (unsigned)pingTarget, (unsigned)r.gs->perPlayerCompTicks[r.s2]);

    {
        LkrOrder road;
        memset(&road, 0, sizeof(road));
        road.build = (uint8_t)(LGM_ROAD_REQUEST + 1);
        road.bx    = destX;
        road.by    = destY;
        lkr_step(&r, &road, NULL);
    }

    /* Walk him down. Each frame, work out where he will be when a shell let
     * off in the next step lands (flight+1 frames of walk from now: the
     * release step's own frame moves him too), and where the server will test
     * him (that minus the owner's pill rewind). Let a shell off the first
     * frame the pair falls the right way round the kill radius. */
    lkr_watch_arm(&watch, NEUTRAL);
    for (i = 0; i < LKR_WATCH_MAX && landedCount < 2; i++) {
        int k;
        if (next < 2 && !(*man)->inTank && !(*man)->isDead &&
            (*man)->state == LGM_STATE_GOING) {
            uint8_t comp = r.gs->perPlayerCompTicks[r.s1];
            int  mx  = (int)(*man)->x;
            int  cy  = (int)(*man)->y + LKR_MAN_SPEED * (flight + 1);
            int  ry  = cy - LKR_MAN_SPEED * (int)comp;
            double dNow = lkr_dist(impactX, impactY, mx, cy);
            double dWas = lkr_dist(impactX, impactY, mx, ry);
            bool go;
            if (next == 0) {
                go = cy <= impactY &&
                     dNow <= LKR_KILL - LKR_PILL_MARGIN &&
                     dWas >= LKR_KILL + LKR_PILL_MARGIN;
            } else {
                go = ry >= impactY &&
                     dWas <= LKR_KILL - LKR_PILL_MARGIN &&
                     dNow >= LKR_KILL + LKR_PILL_MARGIN;
            }
            if (go) {
                r.releasePill = true;
                shots[next].released      = true;
                shots[next].releasePump   = r.pump + 1;
                shots[next].compAtRelease = comp;
                next++;
            }
        }
        lkr_step(&r, NULL, NULL);
        lkr_watch(&r, &watch);
        /* Shells land in the order they were let off: same pill, same target,
         * same flight. */
        for (k = landedCount; k < watch.count && k < 2; k++) {
            if (!watch.s[k].landed) break;
            shots[k].landed = true;
            shots[k].shell  = watch.s[k];
            lkr_man_at(&r, r.s1, r.gs->perPlayerCompTicks[r.s1],
                       (int)watch.s[k].x, (int)watch.s[k].y, &shots[k].at);
            shots[k].diedHere = (*man)->isDead;
            landedCount = k + 1;
        }
        if ((*man)->isDead && landedCount == 0) {
            break;   /* killed by something that was not ours */
        }
        if ((*man)->isDead && next == 1) {
            break;   /* died on shell 1: no point letting off shell 2 */
        }
    }

    /* ── the printout ───────────────────────────────────────────────────── */
    fprintf(stderr, "  pings at the end: owner %ums, target %ums; pill rewind "
                    "for the owner %u frames\n",
            (unsigned)r.h.sim->playerPing[r.s1],
            (unsigned)r.h.sim->playerPing[r.s2],
            (unsigned)r.gs->perPlayerCompTicks[r.s1]);
    for (i = 0; i < 2; i++) {
        LkrPillShot *s = &shots[i];
        if (!s->released) {
            fprintf(stderr, "    %s: never let off\n", s->name);
            continue;
        }
        fprintf(stderr, "    %s: let off pump %d (owner rewind %u then)%s\n",
                s->name, s->releasePump, (unsigned)s->compAtRelease,
                s->landed ? "" : ", never landed");
        if (s->landed) {
            fprintf(stderr, "      landed pump %d, %s\n", s->shell.landPump,
                    s->shell.ranOut ? "RAN OUT (missed the tank)"
                                    : "on the target tank");
            lkr_print_man_at("  it", &s->at, (int)s->shell.x,
                             (int)s->shell.y);
            fprintf(stderr, "      the man %s\n",
                    s->diedHere ? "DIED" : "lived");
        }
    }

    endComp = r.gs->perPlayerCompTicks[r.s1];
    LKR_STOP();   /* r.gs is gone from here on */

    for (i = 0; i < 2; i++) {
        LkrPillShot *s = &shots[i];
        UT_ASSERT_MSG(s->released, "%s: %s was never let off — the man's walk "
                                   "never put him the right way round the kill "
                                   "radius (owner rewind %u frames is too small "
                                   "to separate the two?)",
                      label, s->name, (unsigned)endComp);
        UT_ASSERT_MSG(s->landed && !s->shell.ranOut,
                      "%s: %s did not land on the target tank", label, s->name);
        UT_ASSERT_MSG(s->at.haveCur && s->at.haveRew,
                      "%s: %s — the server's ring had no position for the man",
                      label, s->name);
        UT_ASSERT_MSG(s->at.rewind >= 2,
                      "%s: the owner's pill rewind is %u frames — the link is "
                      "not lagged enough to test it", label,
                      (unsigned)s->at.rewind);
        /* The geometry the release rule aimed for must be what happened,
         * or the outcome below would not say anything. */
        if (i == 0) {
            UT_ASSERT_MSG(s->at.curDist <= LKR_KILL && s->at.rewDist > LKR_KILL,
                          "%s: %s missed its mark: now %.1f, %u frames ago "
                          "%.1f", label, s->name, s->at.curDist,
                          (unsigned)s->at.rewind, s->at.rewDist);
            UT_ASSERT_MSG(!s->diedHere,
                          "%s: %s killed the man although he was %.1f world "
                          "units clear %u frames earlier — the pill hit test "
                          "used his current position (%.1f), not the owner's "
                          "rewind", label, s->name, s->at.rewDist,
                          (unsigned)s->at.rewind, s->at.curDist);
        } else {
            UT_ASSERT_MSG(s->at.curDist > LKR_KILL && s->at.rewDist <= LKR_KILL,
                          "%s: %s missed its mark: now %.1f, %u frames ago "
                          "%.1f", label, s->name, s->at.curDist,
                          (unsigned)s->at.rewind, s->at.rewDist);
            UT_ASSERT_MSG(s->diedHere,
                          "%s: %s did not kill the man although he was %.1f "
                          "world units from it %u frames earlier — the pill "
                          "hit test did not use the owner's rewind", label,
                          s->name, s->at.rewDist, (unsigned)s->at.rewind);
        }
    }
    return 0;
}

/*
 * What the host derives rather than hears: the timers a script sets, the
 * regions it names, and the enter and leave hooks that come from watching
 * where the tanks are.
 *
 * None of these arrives on either of the server's channels. A timer is a
 * function the round holds until a tick it named; a region is a rectangle
 * the round holds until it ends; and the two region hooks are the
 * difference between one tick's picture of the tanks and the last one's.
 * So every case here drives a real round and reads what the script wrote
 * down, because there is no event to publish and nothing else to watch.
 *
 * A script says what happened by appending a line to a file of its own. The
 * name carries the case's name, because ctest runs the cases as concurrent
 * processes in one working directory.
 *
 * Squares are found by the property the case needs — a square a tank can be
 * put on — and a map with none says so rather than the case naming numbers
 * and hoping. The rectangles the region cases name are arbitrary and assume
 * nothing about the map underneath them: a region is arithmetic on two
 * coordinates and does not care what terrain it covers.
 *
 * run_scenario_derived_timer_fires_on_its_tick
 *      — the tick a timer fires on, against the tick it was set on
 * run_scenario_derived_timer_cancelled_and_stale
 *      — cancelling before it is due stops it; cancelling an id that has
 *        already run is harmless and says so
 * run_scenario_derived_timer_limit_boundary
 *      — SCN_TIMERS_MAX wait at once and the one past it is refused
 * run_scenario_derived_timers_die_with_the_round
 *      — a timer still waiting when a round starts does not reach the
 *        round after it
 * run_scenario_derived_region_enter_and_leave
 *      — a tank crossing a declared region, once each way, and nothing
 *        while it stays put
 * run_scenario_derived_define_region_adds_replaces_and_expires
 *      — a defined region, a replacement that costs no room, the limit
 *        shared with the declared ones, and none of it left next round
 * run_scenario_derived_region_loop_terminates
 *      — a handler that moves the tank it was told about is answered on
 *        the following tick, once, and not inside the tick it ran in
 * run_scenario_derived_fixture_wins_without_on_tick
 *      — a script that ends its round with a region hook and a timer and
 *        defines no on_tick at all
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"                /* the terrain codes and the map edges */
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* SetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* the op funnel, for the teleports */
#include "control_event.h"
#include "everard_map.h"
#include "scenario_host.h"
#include "test_harness.h"

/* How many ticks a case will drive before it gives up waiting for
 * something. Every wait here is on the sim's own clock, so this is a bound
 * on a case that has gone wrong rather than a duration. */
#define SD_TICK_LIMIT 600

/* ── Fixtures ─────────────────────────────────────────────────────── */

static void sdSidecarFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SIDECAR_SUFFIX);
}

/* The sidecar: the scenario table the case hands over, the note function
 * every hook writes its line with, and the case's own body. */
static bool sdPut(const char *mapPath, const char *record, const char *table,
                  const char *body) {
    char  side[512];
    char  lua[16384];
    FILE *f;

    snprintf(lua, sizeof(lua),
             "%s\n"
             "local function note(s)\n"
             "  local f = io.open(\"%s\", \"a\")\n"
             "  if f then f:write(s .. \"\\n\") f:close() end\n"
             "end\n"
             "%s", table, record, body);

    sdSidecarFor(mapPath, side, sizeof(side));
    f = fopen(side, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void sdDrop(const char *mapPath) {
    char side[512];
    sdSidecarFor(mapPath, side, sizeof(side));
    remove(side);
}

/* A sim that has not started, ready to be attached to and then started. The
 * host has to be attached first: the round start is where a round's hooks
 * are resolved and its regions read. */
static ServerSim *sdSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    return sim;
}

static void sdRead(const char *record, char *out, size_t outLen) {
    FILE  *f = fopen(record, "rb");
    size_t n;

    out[0] = '\0';
    if (f == NULL) {
        return;
    }
    n = fread(out, 1, outLen - 1, f);
    fclose(f);
    out[n] = '\0';
}

/* How many whole lines the record has that begin with what was asked for.
 *
 * Anchored at the start of a line, and every caller that wants the whole of
 * one passes its newline on the end. Both halves matter: without the anchor
 * a label that ends with another label counts that other one too, which is
 * a trap the first case to write "replace" beside "full_replace" walks
 * straight into — and the name would have said lines while the answer meant
 * substrings. */
static int sdLines(const char *text, const char *line) {
    const char *p     = text;
    size_t      n     = strlen(line);
    int         count = 0;

    if (n == 0) {
        return 0;
    }
    while ((p = strstr(p, line)) != NULL) {
        if (p == text || p[-1] == '\n') {
            count++;
        }
        p += n;
    }
    return count;
}

/* The number on the first line that starts with tag, or -1 when no line
 * does. Anchored the way the counter above is, and for the same reason: a
 * tag another label ends with would otherwise read that other label's
 * number. */
static long sdNumber(const char *text, const char *tag) {
    const char *p = text;
    size_t      n = strlen(tag);

    while ((p = strstr(p, tag)) != NULL) {
        if (p == text || p[-1] == '\n') {
            return strtol(p + n, NULL, 10);
        }
        p += n;
    }
    return -1;
}

/* ── Squares a tank can be put on ─────────────────────────────────── */

/* The teleport handler's own test, asked here so a case names a square by
 * what it needs rather than by a number: inside the mine edges, ground that
 * is not water, a building or a mine, and nothing live standing on it. */
static bool sdDrivable(ServerSim *sim, int x, int y) {
    BYTE t;
    BYTE n, i;

    if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
        y <= MAP_MINE_EDGE_TOP || y >= MAP_MINE_EDGE_BOTTOM) {
        return false;
    }
    t = serverSimGetMapTerrain(sim, (BYTE)x, (BYTE)y);
    if (t != GRASS && t != ROAD && t != RUBBLE && t != SWAMP &&
        t != CRATER && t != FOREST) {
        return false;
    }
    if (serverSimMapIsMine(sim, (BYTE)x, (BYTE)y)) {
        return false;
    }
    n = serverSimGetPillCount(sim);
    for (i = 1; i <= n; i++) {
        ServerSimPillInfo p;
        if (serverSimGetPillInfo(sim, i, &p) && p.active && !p.in_tank &&
            p.x == (BYTE)x && p.y == (BYTE)y) {
            return false;
        }
    }
    return true;
}

/* Two squares a tank can stand on, different from each other. A map with
 * fewer than two says so: every case that moves a tank across a region
 * needs somewhere inside and somewhere outside, and neither is a number
 * this file is entitled to pick. */
static bool sdTwoSquares(ServerSim *sim, BYTE *ax, BYTE *ay, BYTE *bx,
                         BYTE *by) {
    int found = 0;
    int x, y;

    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            if (!sdDrivable(sim, x, y)) {
                continue;
            }
            if (found == 0) {
                *ax = (BYTE)x;
                *ay = (BYTE)y;
                found = 1;
            } else {
                *bx = (BYTE)x;
                *by = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

static ScnOpResult sdTeleport(ServerSim *sim, BYTE slot, BYTE x, BYTE y) {
    ScenarioOp op;

    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_TANK_TELEPORT;
    op.u.tankTeleport.slot   = slot;
    op.u.tankTeleport.mode   = SCN_TELEPORT_SQUARE;
    op.u.tankTeleport.x      = x;
    op.u.tankTeleport.y      = y;
    op.u.tankTeleport.start  = SCN_NONE;
    op.u.tankTeleport.dir    = SCN_NONE;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* Tick until slot 0 has a tank in the world. A player added to a running
 * round is placed over the following frames, and every case that moves a
 * tank needs one to move. */
static bool sdWaitForTank(ServerSim *sim) {
    int i;

    for (i = 0; i < SD_TICK_LIMIT; i++) {
        TankInfo info;
        if (serverSimGetTankInfo(sim, 0, &info) && info.has_tank) {
            return true;
        }
        serverSimTick(sim);
    }
    return false;
}

/* Where slot 0's tank is standing. A case that needs a square to be empty
 * of it asks rather than assuming the placement put it elsewhere. */
static bool sdTankAt(ServerSim *sim, BYTE *mx, BYTE *my) {
    TankInfo info;

    if (!serverSimGetTankInfo(sim, 0, &info) || !info.has_tank) {
        return false;
    }
    *mx = info.map_x;
    *my = info.map_y;
    return true;
}

/* Tick until the record holds the line, or the limit runs out. Answers how
 * many ticks it took, or -1. */
static int sdTickUntil(ServerSim *sim, const char *record, const char *line) {
    char rec[4096];
    int  i;

    for (i = 0; i < SD_TICK_LIMIT; i++) {
        sdRead(record, rec, sizeof(rec));
        if (strstr(rec, line) != NULL) {
            return i;
        }
        serverSimTick(sim);
    }
    return -1;
}

/* ── 1. The tick a timer fires on ─────────────────────────────────── */

/* The script writes down the tick it set the timer on and the tick the
 * timer ran on, and the case holds the difference against the rate the
 * server converts seconds at. A running frame moves the clock two ticks, so
 * the delay lands exactly on a tick a drain runs at: the two are the same
 * parity, which is why this is an equality rather than a window. */
int run_scenario_derived_timer_fires_on_its_tick(void) {
    static const char *const kMap    = "scnder_timer.map";
    static const char *const kRecord = "scnder_timer.txt";
    static const char *const kBody =
        "function on_start()\n"
        "  note(\"set \"..game.tick())\n"
        "  game.timer(1, function() note(\"fired \"..game.tick()) end)\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    char          err[512];
    long          set, fired;

    remove(kRecord);
    UT_ASSERT(sdPut(kMap, kRecord, "scenario = { name = \"T\", api = 1 }",
                    kBody));
    sim = sdSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);

    UT_ASSERT_MSG(sdTickUntil(sim, kRecord, "fired ") >= 0,
                  "the timer never ran in %d ticks", SD_TICK_LIMIT);

    sdRead(kRecord, rec, sizeof(rec));
    set   = sdNumber(rec, "set ");
    fired = sdNumber(rec, "fired ");
    UT_ASSERT_MSG(set >= 0, "the start hook wrote no tick; the record "
                            "was:\n%s", rec);
    UT_ASSERT_MSG(fired == set + GAME_NUMTOTALTICKS_SEC,
                  "the timer was set on tick %ld and ran on %ld, expected "
                  "%ld — one second at %d ticks a second",
                  set, fired, set + GAME_NUMTOTALTICKS_SEC,
                  GAME_NUMTOTALTICKS_SEC);
    UT_ASSERT_MSG(sdLines(rec, "fired ") == 1,
                  "the timer ran %d times; the record was:\n%s",
                  sdLines(rec, "fired "), rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 2. Cancelled, and cancelled again ────────────────────────────── */

/* Three timers. The first is cancelled before it comes due and must never
 * run. The second runs at once. The third, a second later, cancels the
 * second's id — which by then names a timer that has already run, so it
 * finds nothing and says so rather than reaching whatever has since taken
 * the entry it used to sit in. */
int run_scenario_derived_timer_cancelled_and_stale(void) {
    static const char *const kMap    = "scnder_cancel.map";
    static const char *const kRecord = "scnder_cancel.txt";
    static const char *const kBody =
        "function on_start()\n"
        "  local a = game.timer(2, function() note(\"fired_a\") end)\n"
        "  note(\"cancel_a \"..tostring(game.cancel_timer(a)))\n"
        "  local b = game.timer(0, function() note(\"fired_b\") end)\n"
        "  game.timer(1, function()\n"
        "    note(\"stale \"..tostring(game.cancel_timer(b)))\n"
        "  end)\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    char          err[512];
    int           i;

    remove(kRecord);
    UT_ASSERT(sdPut(kMap, kRecord, "scenario = { name = \"C\", api = 1 }",
                    kBody));
    sim = sdSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);

    /* Well past the two seconds the cancelled one was set for. */
    for (i = 0; i < 3 * GAME_NUMTOTALTICKS_SEC; i++) {
        serverSimTick(sim);
    }
    sdRead(kRecord, rec, sizeof(rec));

    UT_ASSERT_MSG(sdLines(rec, "cancel_a true\n") == 1,
                  "cancelling a waiting timer did not report it; the record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "fired_a\n") == 0,
                  "a cancelled timer ran anyway; the record was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "fired_b\n") == 1,
                  "the timer that was not cancelled ran %d times; the record "
                  "was:\n%s", sdLines(rec, "fired_b\n"), rec);
    UT_ASSERT_MSG(sdLines(rec, "stale false\n") == 1,
                  "cancelling an id that had already run did not answer "
                  "false; the record was:\n%s", rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 3. The limit, on both sides of it ────────────────────────────── */

/* Counted in SCN_TIMERS_MAX rather than in a number of its own, so the
 * boundary moves with the constant. The delay is long enough that none of
 * them comes due while the case is running: what is being counted is how
 * many may wait at once. */
int run_scenario_derived_timer_limit_boundary(void) {
    static const char *const kMap    = "scnder_limit.map";
    static const char *const kRecord = "scnder_limit.txt";
    ServerSim    *sim;
    ScenarioHost *h;
    char          body[1024];
    char          rec[4096];
    char          err[512];
    char          want[64];

    remove(kRecord);
    snprintf(body, sizeof(body),
             "function on_start()\n"
             "  local made = 0\n"
             "  for i = 1, %d do\n"
             "    if game.timer(600, function() end) then made = made + 1 end\n"
             "  end\n"
             "  local id, code = game.timer(600, function() end)\n"
             "  note(\"made \"..made)\n"
             "  note(\"extra \"..tostring(id)..\" \"..tostring(code))\n"
             "end\n", SCN_TIMERS_MAX);
    UT_ASSERT(sdPut(kMap, kRecord, "scenario = { name = \"L\", api = 1 }",
                    body));
    sim = sdSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);
    serverSimTick(sim);
    sdRead(kRecord, rec, sizeof(rec));

    snprintf(want, sizeof(want), "made %d\n", SCN_TIMERS_MAX);
    UT_ASSERT_MSG(sdLines(rec, want) == 1,
                  "%d timers were asked for and the script counted %ld set; "
                  "the record was:\n%s", SCN_TIMERS_MAX,
                  sdNumber(rec, "made "), rec);
    UT_ASSERT_MSG(sdLines(rec, "extra nil SCN_OP_FULL\n") == 1,
                  "the timer past the limit was not refused as full; the "
                  "record was:\n%s", rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 4. None outlives its round ───────────────────────────────────── */

/* Each round's start sets one timer a second out. The first round is cut
 * short well before its own comes due, and the second is run past a second.
 * Exactly one timer runs: the second round's. Two would be the first
 * round's function surviving into a round whose VM never held it — which is
 * not merely a stale call but a reference into a state that has been
 * closed. */
int run_scenario_derived_timers_die_with_the_round(void) {
    static const char *const kMap    = "scnder_round.map";
    static const char *const kRecord = "scnder_round.txt";
    static const char *const kBody =
        "function on_start()\n"
        "  note(\"start\")\n"
        "  game.timer(1, function() note(\"fired\") end)\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    char          err[512];
    int           i;

    remove(kRecord);
    UT_ASSERT(sdPut(kMap, kRecord, "scenario = { name = \"R\", api = 1 }",
                    kBody));
    sim = sdSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);

    serverSimStartGame(sim);
    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "start\n") == 1,
                  "the first round did not start once; the record was:\n%s",
                  rec);
    UT_ASSERT_MSG(sdLines(rec, "fired\n") == 0,
                  "the first round's timer ran before it was due; the record "
                  "was:\n%s", rec);

    serverSimStartGame(sim);
    for (i = 0; i < 2 * GAME_NUMTOTALTICKS_SEC; i++) {
        serverSimTick(sim);
    }
    sdRead(kRecord, rec, sizeof(rec));

    UT_ASSERT_MSG(sdLines(rec, "start\n") == 2,
                  "two rounds were started and the start hook ran %d times; "
                  "the record was:\n%s", sdLines(rec, "start\n"), rec);
    UT_ASSERT_MSG(sdLines(rec, "fired\n") == 1,
                  "%d timers ran across two rounds, expected only the second "
                  "round's: the first round's was still waiting when it "
                  "ended. The record was:\n%s", sdLines(rec, "fired\n"), rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 5. A tank crossing a region ──────────────────────────────────── */

/* One region, one square across, on a square a tank can stand on. The tank
 * is put outside it, then inside, then outside again, and the hooks are the
 * two transitions and nothing else: standing still inside a region is not
 * entering it again. */
int run_scenario_derived_region_enter_and_leave(void) {
    static const char *const kMap    = "scnder_cross.map";
    static const char *const kRecord = "scnder_cross.txt";
    static const char *const kBody =
        "function on_enter_region(p, name) note(\"enter \"..p..\" \"..name) end\n"
        "function on_leave_region(p, name) note(\"leave \"..p..\" \"..name) end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          table[256];
    char          rec[4096];
    char          err[512];
    BYTE          ax, ay, bx, by;
    BYTE          tx, ty;
    int           i;

    remove(kRecord);
    sim = sdSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sdTwoSquares(sim, &ax, &ay, &bx, &by),
                  "the map has no two squares a tank can be put on");

    snprintf(table, sizeof(table),
             "scenario = { name = \"X\", api = 1,\n"
             "  regions = { keep = { x = %d, y = %d, w = 1, h = 1 } } }",
             (int)ax, (int)ay);
    UT_ASSERT(sdPut(kMap, kRecord, table, kBody));

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    UT_ASSERT_MSG(sdWaitForTank(sim), "slot 0 never got a tank");
    UT_ASSERT(sdTankAt(sim, &tx, &ty));
    UT_ASSERT_MSG(!(tx == ax && ty == ay),
                  "the placement put the tank on the one square the region "
                  "covers, so the case cannot tell its own entry from that "
                  "one");

    /* Outside first, whatever the placement chose, and one tick to settle
       the scan on it. Only then is the record the case's own. */
    UT_ASSERT(sdTeleport(sim, 0, bx, by) == SCN_OP_OK);
    serverSimTick(sim);
    remove(kRecord);

    UT_ASSERT(sdTeleport(sim, 0, ax, ay) == SCN_OP_OK);
    serverSimTick(sim);
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "enter 0 keep\n") == 1,
                  "the tank moved into the region and the enter hook ran %d "
                  "times; the record was:\n%s",
                  sdLines(rec, "enter 0 keep\n"), rec);
    UT_ASSERT_MSG(sdLines(rec, "leave 0 keep\n") == 0,
                  "the leave hook ran on the way in; the record was:\n%s",
                  rec);

    /* Standing still is not entering again. */
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);
    }
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "enter 0 keep\n") == 1,
                  "the enter hook ran %d times for a tank that had not "
                  "moved; the record was:\n%s",
                  sdLines(rec, "enter 0 keep\n"), rec);

    UT_ASSERT(sdTeleport(sim, 0, bx, by) == SCN_OP_OK);
    serverSimTick(sim);
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "leave 0 keep\n") == 1,
                  "the tank moved out and the leave hook ran %d times; the "
                  "record was:\n%s", sdLines(rec, "leave 0 keep\n"), rec);
    UT_ASSERT_MSG(sdLines(rec, "enter 0 keep\n") == 1,
                  "the enter hook ran again on the way out; the record "
                  "was:\n%s", rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 6. A region a script names ───────────────────────────────────── */

/* Adds one, replaces it by name, fills the round's share up to the limit
 * the declared ones come out of, and finds the limit still lets a
 * replacement through — replacing costs no room. Then a second round, which
 * reads the scenario table over the whole list again and so begins with
 * only what the sidecar declares.
 *
 * The rectangles are the case's own numbers and nothing is assumed about
 * the map underneath them: a region is arithmetic on two coordinates. */
int run_scenario_derived_define_region_adds_replaces_and_expires(void) {
    static const char *const kMap    = "scnder_define.map";
    static const char *const kRecord = "scnder_define.txt";
    ServerSim    *sim;
    ScenarioHost *h;
    char          body[2048];
    char          rec[4096];
    char          err[512];
    char          want[64];

    remove(kRecord);
    snprintf(body, sizeof(body),
             "function on_setup()\n"
             "  note(\"begin \"..#game.regions())\n"
             "  note(\"add \"..tostring(game.define_region(\"d\", 10, 10, 4, 4)))\n"
             "  note(\"in_first \"..tostring(game.in_region(\"d\", 11, 11)))\n"
             "  note(\"replace \"..tostring(game.define_region(\"d\", 100, 100, 2, 2)))\n"
             "  note(\"in_old \"..tostring(game.in_region(\"d\", 11, 11)))\n"
             "  note(\"in_new \"..tostring(game.in_region(\"d\", 100, 100)))\n"
             "  local made = 0\n"
             "  for i = 1, %d do\n"
             "    if game.define_region(\"r\"..i, 0, 0, 1, 1) then\n"
             "      made = made + 1\n"
             "    end\n"
             "  end\n"
             "  note(\"made \"..made)\n"
             "  local ok, code = game.define_region(\"spare\", 0, 0, 1, 1)\n"
             "  note(\"extra \"..tostring(ok)..\" \"..tostring(code))\n"
             "  note(\"full_replace \"..tostring("
             "game.define_region(\"keep\", 5, 5, 3, 3)))\n"
             "  note(\"total \"..#game.regions())\n"
             "end\n", SCN_REGIONS_MAX);
    UT_ASSERT(sdPut(kMap, kRecord,
                    "scenario = { name = \"D\", api = 1,\n"
                    "  regions = { keep = { x = 1, y = 1, w = 2, h = 2 } } }",
                    body));
    sim = sdSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);
    sdRead(kRecord, rec, sizeof(rec));

    UT_ASSERT_MSG(sdLines(rec, "begin 1\n") == 1,
                  "the round did not open with the one declared region; the "
                  "record was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "add true\n") == 1,
                  "defining a region was refused; the record was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "in_first true\n") == 1,
                  "the defined rectangle did not hold a square inside it; "
                  "the record was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "replace true\n") == 1,
                  "redefining the same name was refused; the record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "in_old false\n") == 1,
                  "the replaced rectangle still held its old square: the "
                  "name was duplicated rather than replaced. The record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "in_new true\n") == 1,
                  "the replacement did not take; the record was:\n%s", rec);

    /* One declared and one defined leave room for two short of the limit. */
    snprintf(want, sizeof(want), "made %d\n", SCN_REGIONS_MAX - 2);
    UT_ASSERT_MSG(sdLines(rec, want) == 1,
                  "the round named %ld regions past the two it already had, "
                  "expected %d; the record was:\n%s",
                  sdNumber(rec, "made "), SCN_REGIONS_MAX - 2, rec);
    UT_ASSERT_MSG(sdLines(rec, "extra nil SCN_OP_FULL\n") == 1,
                  "the region past the limit was not refused as full; the "
                  "record was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "full_replace true\n") == 1,
                  "a full round would not replace a region it already had: "
                  "replacing must cost no room. The record was:\n%s", rec);
    snprintf(want, sizeof(want), "total %d\n", SCN_REGIONS_MAX);
    UT_ASSERT_MSG(sdLines(rec, want) == 1,
                  "the round ended up naming %ld regions, expected %d; the "
                  "record was:\n%s", sdNumber(rec, "total "),
                  SCN_REGIONS_MAX, rec);

    /* A second round reads the table again, so what the first defined is
       gone and only the declared one is there. */
    remove(kRecord);
    serverSimStartGame(sim);
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "begin 1\n") == 1,
                  "the second round opened with something other than the one "
                  "declared region: a defined region outlived its round. The "
                  "record was:\n%s", rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 7. A handler that moves what it was told about ───────────────── */

/* The loop these hooks can make: a handler that moves a tank on entry
 * causes another transition. What stops it spinning is where the scan sits,
 * not anything the hook is handed — the picture is taken before any hook
 * runs, so a move a handler makes is seen by the next tick's scan and costs
 * one more call then.
 *
 * That is what this asserts, and it is why the counts are exact. The tank
 * is put inside; the tick after that runs the enter hook, which puts it
 * back outside; and the leave for that move lands on the tick after, not on
 * the same one. A scan that re-read the tanks after running its hooks would
 * report both on the first tick, and one that looped until nothing changed
 * would not return at all. */
int run_scenario_derived_region_loop_terminates(void) {
    static const char *const kMap    = "scnder_loop.map";
    static const char *const kRecord = "scnder_loop.txt";
    ServerSim    *sim;
    ScenarioHost *h;
    char          table[256];
    char          body[512];
    char          rec[4096];
    char          err[512];
    BYTE          ax, ay, bx, by;
    BYTE          tx, ty;
    int           i;

    remove(kRecord);
    sim = sdSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sdTwoSquares(sim, &ax, &ay, &bx, &by),
                  "the map has no two squares a tank can be put on");

    snprintf(table, sizeof(table),
             "scenario = { name = \"P\", api = 1,\n"
             "  regions = { trap = { x = %d, y = %d, w = 1, h = 1 } } }",
             (int)ax, (int)ay);
    snprintf(body, sizeof(body),
             "function on_enter_region(p, name)\n"
             "  note(\"enter\")\n"
             "  game.teleport(p, %d, %d)\n"
             "end\n"
             "function on_leave_region(p, name) note(\"leave\") end\n",
             (int)bx, (int)by);
    UT_ASSERT(sdPut(kMap, kRecord, table, body));

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    UT_ASSERT_MSG(sdWaitForTank(sim), "slot 0 never got a tank");
    UT_ASSERT(sdTankAt(sim, &tx, &ty));
    UT_ASSERT_MSG(!(tx == ax && ty == ay),
                  "the placement put the tank on the one square the region "
                  "covers, so the case cannot tell its own entry from that "
                  "one");

    UT_ASSERT(sdTeleport(sim, 0, bx, by) == SCN_OP_OK);
    serverSimTick(sim);
    remove(kRecord);

    /* In. The next tick runs the enter hook, which puts it straight back
       out — and the leave for that is the tick after, not this one. */
    UT_ASSERT(sdTeleport(sim, 0, ax, ay) == SCN_OP_OK);
    serverSimTick(sim);
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "enter\n") == 1,
                  "the enter hook ran %d times on the tick the tank moved "
                  "in; the record was:\n%s", sdLines(rec, "enter\n"), rec);
    UT_ASSERT_MSG(sdLines(rec, "leave\n") == 0,
                  "the move the handler made was seen inside the tick it was "
                  "made in; the scan must take its picture before the hooks "
                  "run. The record was:\n%s", rec);

    serverSimTick(sim);
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "leave\n") == 1,
                  "the handler's move was not reported on the following "
                  "tick; the record was:\n%s", rec);

    /* And it is over. The tank is outside and nothing moves it again. */
    for (i = 0; i < 10; i++) {
        serverSimTick(sim);
    }
    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "enter\n") == 1 &&
                  sdLines(rec, "leave\n") == 1,
                  "ten quiet ticks produced %d enters and %d leaves; the "
                  "loop did not end. The record was:\n%s",
                  sdLines(rec, "enter\n"), sdLines(rec, "leave\n"), rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

/* ── 8. A round won without an on_tick ────────────────────────────── */

/* The whole point of hooks and timers: a script that says what to do when
 * something happens and when a delay is up, and never asks what tick it is.
 * This one holds a square — a tank standing in the region starts a clock,
 * and when the clock runs out the round is over. It defines no on_tick, and
 * the case asserts the round ended anyway. */
int run_scenario_derived_fixture_wins_without_on_tick(void) {
    static const char *const kMap    = "scnder_win.map";
    static const char *const kRecord = "scnder_win.txt";
    static const char *const kBody =
        "function on_enter_region(p, name)\n"
        "  if name == \"hill\" then\n"
        "    note(\"holding \"..p)\n"
        "    game.timer(0.5, function()\n"
        "      note(\"won\")\n"
        "      game.end_round(\"the hill was held\")\n"
        "    end)\n"
        "  end\n"
        "end\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          table[256];
    char          rec[4096];
    char          err[512];
    BYTE          ax, ay, bx, by;
    BYTE          tx, ty;
    int           i;

    remove(kRecord);
    sim = sdSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sdTwoSquares(sim, &ax, &ay, &bx, &by),
                  "the map has no two squares a tank can be put on");

    snprintf(table, sizeof(table),
             "scenario = { name = \"Hill\", api = 1,\n"
             "  regions = { hill = { x = %d, y = %d, w = 1, h = 1 } } }",
             (int)ax, (int)ay);
    UT_ASSERT(sdPut(kMap, kRecord, table, kBody));

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    UT_ASSERT_MSG(sdWaitForTank(sim), "slot 0 never got a tank");
    UT_ASSERT(sdTankAt(sim, &tx, &ty));
    UT_ASSERT_MSG(!(tx == ax && ty == ay),
                  "the placement put the tank on the one square the region "
                  "covers, so the case cannot tell its own entry from that "
                  "one");

    UT_ASSERT(sdTeleport(sim, 0, bx, by) == SCN_OP_OK);
    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round was over before the case began");

    UT_ASSERT(sdTeleport(sim, 0, ax, ay) == SCN_OP_OK);
    for (i = 0; i < SD_TICK_LIMIT; i++) {
        if (serverSimGetState(sim) != serverStateRunning) {
            break;
        }
        serverSimTick(sim);
    }

    sdRead(kRecord, rec, sizeof(rec));
    UT_ASSERT_MSG(sdLines(rec, "holding 0\n") == 1,
                  "the region hook did not see the tank arrive; the record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(sdLines(rec, "won\n") == 1,
                  "the timer the hook set did not run; the record was:\n%s",
                  rec);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the script did not end its round, and it has no on_tick "
                  "to do it from; the record was:\n%s", rec);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    sdDrop(kMap);
    remove(kRecord);
    return 0;
}

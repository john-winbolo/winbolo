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
 * test_gh_pt2_parity.c -- the pill take v2 evaluator in C
 * (brains/GoalHunter/c/gh_pt2.c) against the Lua copy
 * (pilltake_v2.lua M.evaluate_lua).
 *
 * The test is brains/GoalHunter/test_pt2_c_parity.lua: random small worlds
 * plus the cases recorded in a real game (tests/unit/data/pt2_cases.lua,
 * written by pt2_capture.lua). Each case runs both copies; the result, the
 * order of the calls they make and the terrain reads must be the same.
 * It runs on brains/GoalHunter and on brains/GoalHunter/opt.
 *
 * gh_pt2_engine (below): the shadow's shell walk against the engine's
 * shell and the brain's shot simulator.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "util.h"
#include "shells.h"
#include "brain_pathfinder.h"
#include "gh_pt2.h"
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

static int pt2p_run(const char *brainDir) {
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    lua_pushboolean(L, 0); lua_setglobal(L, "BRAIN_DEBUG_MODE");
    lua_pushboolean(L, 0); lua_setglobal(L, "_PRINT2_ENABLED");
    lua_pushstring(L, brainDir); lua_setglobal(L, "BRAIN_DIR");
    lua_pushstring(L, WB_REPO_ROOT_DIR "/tests/unit/data/pt2_cases.lua");
    lua_setglobal(L, "PT2_CASES_FILE");
    naPt2Register(L);

    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", brainDir, brainDir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    char path[1024];
    snprintf(path, sizeof(path), "%s/test_pt2_c_parity.lua", brainDir);
    int rc = 0;
    if (luaL_loadfile(L, path) != 0) {
        fprintf(stderr, "FAIL load: %s\n", lua_tostring(L, -1));
        rc = 1;
    } else if (lua_pcall(L, 0, 4, 0) != 0) {
        fprintf(stderr, "FAIL run (%s): %s\n", brainDir, lua_tostring(L, -1));
        rc = 1;
    } else {
        int n = (int)lua_tointeger(L, -4);
        int bad = (int)lua_tointeger(L, -3);
        const char *first = lua_tostring(L, -2);
        const char *cov = lua_tostring(L, -1);
        printf("gh_pt2_parity (%s): %d cases, %d mismatches; %s\n", brainDir, n, bad,
               cov ? cov : "");
        if (bad > 0) {
            fprintf(stderr, "FAIL first mismatch: %s\n", first ? first : "");
            rc = 1;
        }
        if (n == 0) {
            fprintf(stderr, "FAIL no case ran\n");
            rc = 1;
        }
    }
    lua_close(L);
    return rc;
}

/* ── The shadow's shell walk against the engine's shell ─────────────────
 * Andrew, Nov 18: the rays must follow the engine's shell exactly. For
 * every ring point round a pill (and round the pills next to it), the
 * walk gh_pt2.c uses (naPt2TestWalk) must give the same angle, the same
 * squares on the same steps and the same tank-hit step as the engine's
 * own shell code: utilCalcAngle (pillsTargetTank), shellSpawnPos,
 * shellLifeTicks, shellAdvance1Tick (shellsUpdate) and the circle of
 * tankShellInHitZone. The brain's shot simulator
 * (brainPathfinderSimulateShotAngle) must give the same squares too, once
 * its two first entries (the shooter's square, the spawn square) are put
 * the same way. For our own shells at the pill (tank_shot) the same, with
 * the tank's angle (shellAngleFromTarget) and brainPathfinderSimulateShot.
 * simulate_shot_walk_tanks (a 128 box, not the circle) is only counted. */

#define PT2E_MAX 96

typedef struct { int mx[PT2E_MAX], my[PT2E_MAX], t[PT2E_MAX]; int n; int hit_t; float angle;
                 int smx, smy; } Pt2EWalk;

/* The engine's shell: the squares it enters (step 1 on) and the first step
 * inside the circle of radius hitR round (tx, ty) (0 = none). */
static void pt2e_engine(int kind, const double sh[5], WORLD sx, WORLD sy, WORLD tx, WORLD ty,
                        Pt2EWalk *o) {
    int speed = (int)sh[0], add = (int)sh[1], life = (int)sh[2], hitR = (int)sh[3];
    float len = kind == 0 ? (float)sh[4] : 7.0f;
    TURNTYPE a = kind == 0 ? utilCalcAngle(sx, sy, tx, ty) : shellAngleFromTarget(sx, sy, tx, ty);
    WORLD x, y;
    shellSpawnPos(sx, sy, a, speed, add, &x, &y);
    o->smx = x >> 8; o->smy = y >> 8;
    int ticks = shellLifeTicks(len, life, add);
    int32_t xs = 0, ys = 0, xa = 0, ya = 0;
    utilCalcDistanceHP(&xs, &ys, a, speed);
    int last = -1;
    o->n = 0; o->hit_t = 0; o->angle = a;
    for (int t = 1; t <= ticks; t++) {
        shellAdvance1Tick(&x, &y, &xa, &ya, xs, ys);
        int mx = x >> 8, my = y >> 8, sq = my * 256 + mx;
        if (sq != last) {
            last = sq;
            if (o->n < PT2E_MAX) { o->mx[o->n] = mx; o->my[o->n] = my; o->t[o->n] = t; }
            o->n++;
        }
        if (kind == 0 && o->hit_t == 0) {
            int dx = (int)tx - (int)x, dy = (int)ty - (int)y;
            if (abs(dx) < hitR && abs(dy) < hitR && dx * dx + dy * dy < hitR * hitR) o->hit_t = t;
        }
    }
}

/* A walk's squares the way simulate_shot_walk lists them: the shooter's
 * square, the spawn square when it differs, then each new square. */
static int pt2e_sim_style(WORLD sx, WORLD sy, const Pt2EWalk *w, BrainShotTile *out, int max) {
    int n = 0, lx, ly;
    out[n].mx = (uint8_t)(sx >> 8); out[n].my = (uint8_t)(sy >> 8); n++;
    lx = sx >> 8; ly = sy >> 8;
    if (w->smx != lx || w->smy != ly) {
        out[n].mx = (uint8_t)w->smx; out[n].my = (uint8_t)w->smy; n++;
        lx = w->smx; ly = w->smy;
    }
    for (int k = 0; k < w->n && k < PT2E_MAX && n < max; k++) {
        if (w->mx[k] == lx && w->my[k] == ly) continue;
        out[n].mx = (uint8_t)w->mx[k]; out[n].my = (uint8_t)w->my[k]; n++;
        lx = w->mx[k]; ly = w->my[k];
    }
    return n;
}

static int pt2e_same_tiles(const BrainShotTile *a, int na, const BrainShotTile *b, int nb) {
    if (na != nb) return 0;
    for (int k = 0; k < na; k++)
        if (a[k].mx != b[k].mx || a[k].my != b[k].my) return 0;
    return 1;
}

static int pt2e_run(const double sh[5], const char *name) {
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetShellRules(pf, (int32_t)sh[2], (int32_t)sh[0], (int32_t)sh[1], 14);
    static const int PO[4][2] = { { 0, 0 }, { 2, 1 }, { -3, 2 }, { 1, -4 } };
    const int amx = 120, amy = 120;
    int rays = 0, bad_walk = 0, bad_sim = 0, bad_box = 0;
    int shots = 0, bad_tangle = 0, bad_tangle_sq = 0, bad_twalk = 0, bad_tsim = 0;
    char first[256] = "";
    for (int p = 0; p < 4; p++) {
        WORLD px = (WORLD)((amx + PO[p][0]) * 256 + 128), py = (WORLD)((amy + PO[p][1]) * 256 + 128);
        for (int ri = 0; ri < 8; ri++) {
            for (int oi = 0; oi < 2; oi++) {
                double R = (5.0 + ri * 0.5 + (oi ? -0.4 : 0.0)) * 256.0;
                for (int deg = 0; deg < 360; deg += 2) {
                    double rad = deg * 3.14159265358979323846 / 180.0;
                    WORLD tx = (WORLD)((amx * 256 + 128) + (int)floor(R * sin(rad)));
                    WORLD ty = (WORLD)((amy * 256 + 128) - (int)floor(R * cos(rad)));
                    if (tx == px && ty == py) continue;
                    /* the pill's shell at the tank point */
                    Pt2EWalk eng, gh;
                    pt2e_engine(0, sh, px, py, tx, ty, &eng);
                    double ang;
                    gh.n = naPt2TestWalk(0, sh, px, py, tx, ty, &ang, gh.mx, gh.my, gh.t, PT2E_MAX, &gh.hit_t);
                    gh.angle = (float)ang;
                    rays++;
                    int same = ang == (double)eng.angle && gh.n == eng.n && gh.hit_t == eng.hit_t;
                    for (int k = 0; same && k < gh.n && k < PT2E_MAX; k++)
                        same = gh.mx[k] == eng.mx[k] && gh.my[k] == eng.my[k] && gh.t[k] == eng.t[k];
                    if (!same) {
                        bad_walk++;
                        if (!first[0]) snprintf(first, sizeof(first),
                            "pill ray %u,%u to %u,%u: angle %.9g / %.9g, squares %d / %d, hit step %d / %d",
                            px, py, tx, ty, ang, (double)eng.angle, gh.n, eng.n, gh.hit_t, eng.hit_t);
                    }
                    BrainShotTile sim[PT2E_MAX], mine[PT2E_MAX];
                    int ns = brainPathfinderSimulateShotAngle(pf, px, py, eng.angle, BRAIN_SHOT_SHOOTER_PILL, 0,
                                                              sim, PT2E_MAX);
                    int nm = pt2e_sim_style(px, py, &eng, mine, PT2E_MAX);
                    if (!pt2e_same_tiles(sim, ns, mine, nm)) bad_sim++;
                    /* simulate_shot_walk_tanks: the first tank entry */
                    BrainShotTankPos tp = { tx, ty, 3 };
                    BrainShotTile wt[PT2E_MAX];
                    int nw = brainPathfinderSimulateShotWithTanks(pf, px, py, tx, ty, BRAIN_SHOT_SHOOTER_PILL, 0,
                                                                  &tp, 1, 200, wt, PT2E_MAX);
                    int box_hit = 0;
                    for (int k = 0; k < nw; k++) if (wt[k].hit_type == BRAIN_SHOT_HIT_TANK) box_hit = 1;
                    if (box_hit != (eng.hit_t > 0)) bad_box++;
                    /* our shell from the tank point at the pill (tank_shot) */
                    Pt2EWalk te, tg;
                    pt2e_engine(1, sh, tx, ty, px, py, &te);
                    tg.n = naPt2TestWalk(1, sh, tx, ty, px, py, &ang, tg.mx, tg.my, tg.t, PT2E_MAX, NULL);
                    shots++;
                    int ts = tg.n == te.n;
                    for (int k = 0; ts && k < tg.n && k < PT2E_MAX; k++)
                        ts = tg.mx[k] == te.mx[k] && tg.my[k] == te.my[k] && tg.t[k] == te.t[k];
                    if (ang != (double)te.angle) {
                        /* double atan2 (shadow.lua) against atan2f: report only */
                        bad_tangle++;
                        if (!ts) bad_tangle_sq++;
                    } else {
                        if (!ts) {
                            bad_twalk++;
                            if (!first[0]) snprintf(first, sizeof(first), "tank shot %u,%u at %u,%u: squares differ",
                                                    tx, ty, px, py);
                        }
                    }
                    int ns2 = brainPathfinderSimulateShot(pf, tx, ty, px, py, BRAIN_SHOT_SHOOTER_TANK, 14,
                                                          sim, PT2E_MAX);
                    int nm2 = pt2e_sim_style(tx, ty, &te, mine, PT2E_MAX);
                    if (!pt2e_same_tiles(sim, ns2, mine, nm2)) bad_tsim++;
                }
            }
        }
    }
    brainPathfinderDestroy(pf);
    printf("gh_pt2_engine (%s): %d pill rays: walk differs %d, shot sim differs %d, "
           "128 box tank test differs %d (report only); %d tank shots: angle differs %d "
           "(report only; squares differ in %d of them), "
           "walk differs %d, shot sim differs %d\n",
           name, rays, bad_walk, bad_sim, bad_box, shots, bad_tangle, bad_tangle_sq, bad_twalk, bad_tsim);
    if (first[0]) printf("  first: %s\n", first);
    if (bad_walk || bad_sim || bad_twalk || bad_tsim) {
        fprintf(stderr, "FAIL gh_pt2_engine (%s): the shadow's walk is not the engine's shell\n", name);
        return 1;
    }
    return 0;
}

static int pt2e_all(void) {
    /* speed, start add, life, tank hit radius, pill fire length */
    static const double classic[5] = { 32, 5, 8, 112, 8.5 };
    static const double tuned[5]   = { 40, 3, 7, 128, 8.5 };
    static const double slow[5]    = { 24, 6, 9, 96, 8.5 };
    int rc = pt2e_run(classic, "classic");
    rc |= pt2e_run(tuned, "speed 40 add 3 life 7 r 128");
    rc |= pt2e_run(slow, "speed 24 add 6 life 9 r 96");
    return rc;
}

int run_gh_pt2_parity(void) {
    int rc = pt2e_all();
    rc |= pt2p_run(WB_REPO_ROOT_DIR "/brains/GoalHunter");
    rc |= pt2p_run(WB_REPO_ROOT_DIR "/brains/GoalHunter/opt");
    return rc;
}

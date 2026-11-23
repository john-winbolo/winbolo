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
 * test_gh_pt2_old_parity.c -- the pill take v2 evaluator in C
 * (brains/GoalHunter/c/gh_pt2_old.c) against the Lua copy
 * (pilltake_v2_old.lua M.evaluate_lua).
 *
 * The test is brains/GoalHunter/test_pt2_old_c_parity.lua: random small worlds
 * plus the cases recorded in a real game (tests/unit/data/pt2_cases.lua,
 * written by pt2_capture.lua). Each case runs both copies; the result, the
 * order of the calls they make and the terrain reads must be the same.
 * It runs on brains/GoalHunter and on brains/GoalHunter/opt.
 *
 * gh_pt2_old_engine (below): the shadow's shell walk against the engine's
 * shell and the brain's shot simulator.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "util.h"
#include "shells.h"
#include "brain_pathfinder.h"
#include "gh_pt2_old.h"
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
    naPt2OldRegister(L);

    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", brainDir, brainDir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    char path[1024];
    snprintf(path, sizeof(path), "%s/test_pt2_old_c_parity.lua", brainDir);
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
        printf("gh_pt2_old_parity (%s): %d cases, %d mismatches; %s\n", brainDir, n, bad,
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
 * walk gh_pt2_old.c uses (naPt2OldTestWalk) must give the same angle, the same
 * squares on the same steps and the same tank-hit step as the engine's
 * own shell code: utilCalcAngle (pillsTargetTank), shellSpawnPos,
 * shellLifeTicks, shellAdvance1Tick (shellsUpdate) and the circle of
 * tankShellInHitZone. The brain's shot simulator
 * (brainPathfinderSimulateShotAngle) must give the same squares too, once
 * its two first entries (the shooter's square, the spawn square) are put
 * the same way. For our own shells at the pill (tank_shot) the same, with
 * the tank's angle (shellAngleFromTarget) and brainPathfinderSimulateShot.
 *
 * Andrew, Nov 19: the shot simulator must match the engine exactly, with
 * separate rules for a pill's shell and a tank's shell. So also, on all
 * three rule sets (each with its own pill_fire_length): the pill's
 * brainPathfinderSimulateShot at a target point (utilCalcAngle,
 * pill_fire_length), the tank's at sightLen 14 and 13 (the float half
 * square), and brainPathfinderSimulateShotWithTanks for both shooters
 * against tanks on, inside, on the edge of and outside the hit zone
 * (utilIsTankHit, the test tankShellInHitZone makes), with the owner's
 * tank skipped. 0 differences. The old answers (SHOT_SIM_ENGINE_EXACT
 * false) are only counted. A build with BOLO_LEGACY_SQUARE_COLLISION runs
 * the same test on the 128 box, since both sides call utilIsTankHit. */

#define PT2E_MAX 96
#define PT2E_TICKS 192

typedef struct { int mx[PT2E_MAX], my[PT2E_MAX], t[PT2E_MAX]; int n; int hit_t; float angle;
                 int smx, smy; int ticks; WORLD x[PT2E_TICKS + 1], y[PT2E_TICKS + 1]; } Pt2EWalk;

/* The engine's shell: the squares it enters (step 1 on), its place on each
 * step, and the first step inside the hit zone round (tx, ty) (0 = none).
 * kind 0 is a pill's shell (utilCalcAngle, the length handed in), kind 1 a
 * tank's (shellAngleFromTarget, the length handed in). The hit zone is
 * utilIsTankHit, the test tankShellInHitZone makes. */
static void pt2e_engine(int kind, const double sh[5], float len, WORLD sx, WORLD sy, WORLD tx, WORLD ty,
                        Pt2EWalk *o) {
    int speed = (int)sh[0], add = (int)sh[1], life = (int)sh[2], hitR = (int)sh[3];
    TURNTYPE a = kind == 0 ? utilCalcAngle(sx, sy, tx, ty) : shellAngleFromTarget(sx, sy, tx, ty);
    WORLD x, y;
    shellSpawnPos(sx, sy, a, speed, add, &x, &y);
    o->smx = x >> 8; o->smy = y >> 8;
    int ticks = shellLifeTicks(len, life, add);
    if (ticks > PT2E_TICKS) ticks = PT2E_TICKS;  /* the rule sets below stay far under */
    int32_t xs = 0, ys = 0, xa = 0, ya = 0;
    utilCalcDistanceHP(&xs, &ys, a, speed);
    int last = -1;
    o->n = 0; o->hit_t = 0; o->angle = a; o->ticks = ticks;
    for (int t = 1; t <= ticks; t++) {
        shellAdvance1Tick(&x, &y, &xa, &ya, xs, ys);
        o->x[t] = x; o->y[t] = y;
        int mx = x >> 8, my = y >> 8, sq = my * 256 + mx;
        if (sq != last) {
            last = sq;
            if (o->n < PT2E_MAX) { o->mx[o->n] = mx; o->my[o->n] = my; o->t[o->n] = t; }
            o->n++;
        }
        if (o->hit_t == 0 && utilIsTankHit(tx, ty, 0, x, y, 0, (WORLD)hitR)) o->hit_t = t;
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

/* What simulate_shot_walk_tanks must give for the engine's shell w among
 * these tanks: the sim_style squares up to the first step a tank that is
 * not the owner's is in the hit zone (utilIsTankHit, tank order), then
 * that hit entry. */
static int pt2e_with_tanks(WORLD sx, WORLD sy, const Pt2EWalk *w, const double sh[5],
                           const BrainShotTankPos *tp, int nt, int owner,
                           BrainShotTile *out, int max) {
    int n = 0, lx, ly;
    out[n].mx = (uint8_t)(sx >> 8); out[n].my = (uint8_t)(sy >> 8);
    out[n].hit_type = BRAIN_SHOT_HIT_TILE; out[n].hit_id = 0; n++;
    lx = sx >> 8; ly = sy >> 8;
    if (w->smx != lx || w->smy != ly) {
        out[n].mx = (uint8_t)w->smx; out[n].my = (uint8_t)w->smy;
        out[n].hit_type = BRAIN_SHOT_HIT_TILE; out[n].hit_id = 0; n++;
        lx = w->smx; ly = w->smy;
    }
    for (int t = 1; t <= w->ticks && n < max; t++) {
        int mx = w->x[t] >> 8, my = w->y[t] >> 8;
        for (int i = 0; i < nt; i++) {
            if (tp[i].player_num == owner) continue;
            if (utilIsTankHit(tp[i].wx, tp[i].wy, 0, w->x[t], w->y[t], 0, (WORLD)sh[3])) {
                out[n].mx = (uint8_t)mx; out[n].my = (uint8_t)my;
                out[n].hit_type = BRAIN_SHOT_HIT_TANK; out[n].hit_id = tp[i].player_num;
                return n + 1;
            }
        }
        if (mx != lx || my != ly) {
            out[n].mx = (uint8_t)mx; out[n].my = (uint8_t)my;
            out[n].hit_type = BRAIN_SHOT_HIT_TILE; out[n].hit_id = 0; n++;
            lx = mx; ly = my;
        }
    }
    return n;
}

static int pt2e_same_tiles(const BrainShotTile *a, int na, const BrainShotTile *b, int nb) {
    if (na != nb) return 0;
    for (int k = 0; k < na; k++)
        if (a[k].mx != b[k].mx || a[k].my != b[k].my) return 0;
    return 1;
}

static int pt2e_same_hits(const BrainShotTile *a, int na, const BrainShotTile *b, int nb) {
    if (!pt2e_same_tiles(a, na, b, nb)) return 0;
    for (int k = 0; k < na; k++)
        if (a[k].hit_type != b[k].hit_type || a[k].hit_id != b[k].hit_id) return 0;
    return 1;
}

/* The tanks round a target point for the tank tests: one on the point,
 * three near the edge of the hit zone (inside, on it, outside), one the
 * owner's own (it must be skipped). */
static int pt2e_tanks(WORLD x, WORLD y, int hitR, int which, int owner, BrainShotTankPos *tp) {
    static const int OFF[4][2] = { { 0, 0 }, { 1, 1 }, { -1, 0 }, { 1, -1 } };
    int d = which == 0 ? 0 : (which == 1 ? hitR * 7 / 10 : (which == 2 ? hitR : hitR * 3 / 2));
    int o = which % 4;
    tp[0].wx = (WORLD)((int)x + OFF[o][0] * d); tp[0].wy = (WORLD)((int)y + OFF[o][1] * d);
    tp[0].player_num = 3;
    tp[1].wx = x; tp[1].wy = y; tp[1].player_num = (uint8_t)owner;
    return 2;
}

static int pt2e_run(const double sh[5], const char *name) {
    BrainPathfinder *pf = brainPathfinderCreate();
    UT_ASSERT(pf != NULL);
    brainPathfinderSetShellRules(pf, (int32_t)sh[2], (int32_t)sh[0], (int32_t)sh[1], 14);
    brainPathfinderSetShotRules(pf, (float)sh[4], (int32_t)sh[3]);
    static const int PO[4][2] = { { 0, 0 }, { 2, 1 }, { -3, 2 }, { 1, -4 } };
    static const int SL[2] = { 14, 13 };
    const int amx = 120, amy = 120;
    const int hitR = (int)sh[3];
    int rays = 0, bad_walk = 0, bad_sim = 0, bad_psim = 0, bad_pwt = 0, pwt = 0;
    int shots = 0, bad_tangle = 0, bad_tangle_sq = 0, bad_twalk = 0, bad_tsim = 0, bad_twt = 0, twt = 0;
    int old_psim = 0, old_pwt = 0, old_tsim = 0, old_twt = 0;
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
                    int which = (deg / 2 + ri + oi) % 4;
                    /* the pill's shell at the tank point */
                    Pt2EWalk eng, gh;
                    pt2e_engine(0, sh, (float)sh[4], px, py, tx, ty, &eng);
                    double ang;
                    gh.n = naPt2OldTestWalk(0, sh, px, py, tx, ty, &ang, gh.mx, gh.my, gh.t, PT2E_MAX, &gh.hit_t);
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
                    BrainShotTile sim[PT2E_MAX], mine[PT2E_MAX], old[PT2E_MAX];
                    int ns = brainPathfinderSimulateShotAngle(pf, px, py, eng.angle, BRAIN_SHOT_SHOOTER_PILL, 0,
                                                              sim, PT2E_MAX);
                    int nm = pt2e_sim_style(px, py, &eng, mine, PT2E_MAX);
                    if (!pt2e_same_tiles(sim, ns, mine, nm)) {
                        bad_sim++;
                        if (!first[0]) snprintf(first, sizeof(first), "pill shot sim (angle %.9g) %u,%u at %u,%u: "
                                                "squares differ", (double)eng.angle, px, py, tx, ty);
                    }
                    /* the pill's shot sim at a target point: utilCalcAngle */
                    ns = brainPathfinderSimulateShot(pf, px, py, tx, ty, BRAIN_SHOT_SHOOTER_PILL, 0, sim, PT2E_MAX);
                    if (!pt2e_same_tiles(sim, ns, mine, nm)) {
                        bad_psim++;
                        if (!first[0]) snprintf(first, sizeof(first), "pill shot sim %u,%u at %u,%u: squares differ",
                                                px, py, tx, ty);
                    }
                    brainPathfinderSetShotSimExact(pf, 0);
                    int no = brainPathfinderSimulateShot(pf, px, py, tx, ty, BRAIN_SHOT_SHOOTER_PILL, 0, old, PT2E_MAX);
                    brainPathfinderSetShotSimExact(pf, 1);
                    if (!pt2e_same_tiles(sim, ns, old, no)) old_psim++;
                    /* simulate_shot_walk_tanks for the pill's shell */
                    BrainShotTankPos tp[2];
                    int nt = pt2e_tanks(tx, ty, hitR, which, 200, tp);
                    BrainShotTile wt[PT2E_MAX], wx[PT2E_MAX];
                    int nw = brainPathfinderSimulateShotWithTanks(pf, px, py, tx, ty, BRAIN_SHOT_SHOOTER_PILL, 0,
                                                                  tp, nt, 200, wt, PT2E_MAX);
                    int nx = pt2e_with_tanks(px, py, &eng, sh, tp, nt, 200, wx, PT2E_MAX);
                    pwt++;
                    if (!pt2e_same_hits(wt, nw, wx, nx)) {
                        bad_pwt++;
                        if (!first[0]) snprintf(first, sizeof(first), "pill shot with tanks %u,%u at %u,%u: "
                                                "entries %d / %d", px, py, tx, ty, nw, nx);
                    }
                    brainPathfinderSetShotSimExact(pf, 0);
                    no = brainPathfinderSimulateShotWithTanks(pf, px, py, tx, ty, BRAIN_SHOT_SHOOTER_PILL, 0,
                                                              tp, nt, 200, old, PT2E_MAX);
                    brainPathfinderSetShotSimExact(pf, 1);
                    if (!pt2e_same_hits(wt, nw, old, no)) old_pwt++;
                    /* our shell from the tank point at the pill (tank_shot) */
                    Pt2EWalk te, tg;
                    pt2e_engine(1, sh, 7.0f, tx, ty, px, py, &te);
                    tg.n = naPt2OldTestWalk(1, sh, tx, ty, px, py, &ang, tg.mx, tg.my, tg.t, PT2E_MAX, NULL);
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
                    for (int si = 0; si < 2; si++) {
                        Pt2EWalk tl;
                        pt2e_engine(1, sh, (TURNTYPE)SL[si] / (TURNTYPE)2, tx, ty, px, py, &tl);
                        int ns2 = brainPathfinderSimulateShot(pf, tx, ty, px, py, BRAIN_SHOT_SHOOTER_TANK, SL[si],
                                                              sim, PT2E_MAX);
                        int nm2 = pt2e_sim_style(tx, ty, &tl, mine, PT2E_MAX);
                        if (!pt2e_same_tiles(sim, ns2, mine, nm2)) bad_tsim++;
                        brainPathfinderSetShotSimExact(pf, 0);
                        no = brainPathfinderSimulateShot(pf, tx, ty, px, py, BRAIN_SHOT_SHOOTER_TANK, SL[si],
                                                         old, PT2E_MAX);
                        brainPathfinderSetShotSimExact(pf, 1);
                        if (!pt2e_same_tiles(sim, ns2, old, no)) old_tsim++;
                        /* simulate_shot_walk_tanks for our shell at a tank by the pill */
                        nt = pt2e_tanks(px, py, hitR, which, 1, tp);
                        tp[1].wx = tx; tp[1].wy = ty;  /* the owner sits on the shooter's point */
                        nw = brainPathfinderSimulateShotWithTanks(pf, tx, ty, px, py, BRAIN_SHOT_SHOOTER_TANK, SL[si],
                                                                  tp, nt, 1, wt, PT2E_MAX);
                        nx = pt2e_with_tanks(tx, ty, &tl, sh, tp, nt, 1, wx, PT2E_MAX);
                        twt++;
                        if (!pt2e_same_hits(wt, nw, wx, nx)) {
                            bad_twt++;
                            if (!first[0]) snprintf(first, sizeof(first), "tank shot with tanks %u,%u at %u,%u "
                                                    "sight %d: entries %d / %d", tx, ty, px, py, SL[si], nw, nx);
                        }
                        brainPathfinderSetShotSimExact(pf, 0);
                        no = brainPathfinderSimulateShotWithTanks(pf, tx, ty, px, py, BRAIN_SHOT_SHOOTER_TANK, SL[si],
                                                                  tp, nt, 1, old, PT2E_MAX);
                        brainPathfinderSetShotSimExact(pf, 1);
                        if (!pt2e_same_hits(wt, nw, old, no)) old_twt++;
                    }
                }
            }
        }
    }
    brainPathfinderDestroy(pf);
    printf("gh_pt2_old_engine (%s): %d pill rays: walk differs %d, shot sim (angle) differs %d, "
           "shot sim (target) differs %d, with tanks differs %d of %d; %d tank shots: angle differs %d "
           "(report only; squares differ in %d of them), walk differs %d, shot sim differs %d (sight 14 and 13), "
           "with tanks differs %d of %d\n",
           name, rays, bad_walk, bad_sim, bad_psim, bad_pwt, pwt, shots, bad_tangle, bad_tangle_sq,
           bad_twalk, bad_tsim, bad_twt, twt);
    printf("  old answers (SHOT_SIM_ENGINE_EXACT false) differ from the engine-exact ones: pill shot sim %d of %d, "
           "pill with tanks %d of %d, tank shot sim %d of %d, tank with tanks %d of %d\n",
           old_psim, rays, old_pwt, pwt, old_tsim, twt, old_twt, twt);
    if (first[0]) printf("  first: %s\n", first);
    if (bad_walk || bad_sim || bad_psim || bad_pwt || bad_twalk || bad_tsim || bad_twt) {
        fprintf(stderr, "FAIL gh_pt2_old_engine (%s): a shell walk is not the engine's shell\n", name);
        return 1;
    }
    return 0;
}

static int pt2e_all(void) {
    /* speed, start add, life, tank hit radius, pill fire length */
    static const double classic[5] = { 32, 5, 8, 112, 8.5 };
    static const double tuned[5]   = { 40, 3, 7, 128, 10.25 };
    static const double slow[5]    = { 24, 6, 9, 96, 6.5 };
#ifdef BOLO_LEGACY_SQUARE_COLLISION
    printf("gh_pt2_old_engine: built with BOLO_LEGACY_SQUARE_COLLISION (128 box hit zone)\n");
#endif
    int rc = pt2e_run(classic, "classic");
    rc |= pt2e_run(tuned, "speed 40 add 3 life 7 r 128 pill length 10.25");
    rc |= pt2e_run(slow, "speed 24 add 6 life 9 r 96 pill length 6.5");
    return rc;
}

int run_gh_pt2_old_parity(void) {
    int rc = pt2e_all();
    rc |= pt2p_run(WB_REPO_ROOT_DIR "/brains/GoalHunter");
    rc |= pt2p_run(WB_REPO_ROOT_DIR "/brains/GoalHunter/opt");
    return rc;
}

/* gh_pt2_bench: a speed check of the evaluator on recorded inputs (no pass
 * or fail of its own). It runs only when WB_PT2_BENCH names a Lua script
 * (tests/unit/pt2_bench.lua); else it does nothing. WB_PT2_BENCH_BRAIN is
 * the brain folder (default brains/GoalHunter) and WB_PT2_BENCH_ARGS is
 * handed to the script as the global PT2_BENCH_ARGS. The script returns
 * 0 when it ran. */
/* bench_clock_us(): a wall clock in microseconds for the script (the
 * test state has no ffi). */
static int pt2b_clock_us(lua_State *L) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    lua_pushnumber(L, (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3);
    return 1;
}

int run_gh_pt2_bench(void) {
    const char *script = getenv("WB_PT2_BENCH");
    if (!script || !script[0]) {
        printf("gh_pt2_bench: WB_PT2_BENCH is not set: skipped\n");
        return 0;
    }
    const char *brainDir = getenv("WB_PT2_BENCH_BRAIN");
    if (!brainDir || !brainDir[0]) brainDir = WB_REPO_ROOT_DIR "/brains/GoalHunter";
    const char *args = getenv("WB_PT2_BENCH_ARGS");
    lua_State *L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);
    lua_pushboolean(L, 0); lua_setglobal(L, "BRAIN_DEBUG_MODE");
    lua_pushboolean(L, 0); lua_setglobal(L, "_PRINT2_ENABLED");
    lua_pushstring(L, brainDir); lua_setglobal(L, "BRAIN_DIR");
    lua_pushstring(L, args ? args : ""); lua_setglobal(L, "PT2_BENCH_ARGS");
    lua_pushcfunction(L, pt2b_clock_us); lua_setglobal(L, "bench_clock_us");
    naPt2OldRegister(L);
    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua;%s/?/init.lua", brainDir, brainDir);
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
    int rc = 0;
    if (luaL_loadfile(L, script) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        fprintf(stderr, "FAIL gh_pt2_bench: %s\n", lua_tostring(L, -1));
        rc = 1;
    } else if (lua_tointeger(L, -1) != 0) {
        rc = 1;
    }
    lua_close(L);
    return rc;
}

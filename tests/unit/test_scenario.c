/*
 * Scripted map scenarios (src/server/scenario.c).
 *
 * A map "X.map" with a companion "X.scenario.lua" boots a server-side
 * Lua VM. Pinned here:
 *
 *   1. Sidecar discovery: a sidecar next to the map file activates the
 *      scenario and its metadata (scenario.max_players) is captured; a
 *      plain map stays scenario-free; a sidecar that fails to compile
 *      leaves the sim running as a plain map instead of failing create.
 *   2. on_start fires on the round's FIRST running tick — not at sim
 *      create, not at StartGame — and exactly once.
 *   3. Round restart = fresh VM: a second serverSimStartGame on the
 *      same map re-runs the chunk and on_start with virgin globals, so
 *      round 2 can never inherit round 1's wave counters / end flags.
 *   4. The player cap follows the map: scenario.max_players clamps
 *      serverSimGetMaxPlayers + serverSimFindFreeSlot while the
 *      scripted map is loaded, a lobby switch to a plain map lifts it,
 *      and cancelling that switch (revert preview) restores both the
 *      scenario and the cap.
 *   5. game.end_round(text) ends the round through the game-over path
 *      tagged RETURN_REASON_SCENARIO with the script's text as the win
 *      message.
 *   6. game.set_base_owner / game.set_pill_owner mutate the engine's
 *      own structures.
 *   7. A persistently-erroring on_tick disables the scenario after
 *      SCENARIO_MAX_ERRORS consecutive failures instead of wedging or
 *      crashing the sim — observed by counting how many ticks still
 *      reached the script.
 *   8. game.give_pill loads a pill into a live tank exactly once —
 *      1-based like the rest of the pills* API (the engine setters
 *      silently no-op on a 0-based number, which is how the survival
 *      map briefly shipped 15/16 pills) — and refuses re-gives and
 *      out-of-range numbers.
 *   9. scenario.game forces the sim's game type at create AND re-forces
 *      it at round start, beating a lobby-set type, so the round's
 *      tanks spawn under the scripted rules.
 *
 * Fixture maps are written at runtime with mapWrite from an Everard
 * Island sim (E_MAP), so the tests need no committed .map files and a
 * per-test sidecar is just an fprintf.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* returnToLobbyReason, state poke */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_join.h"       /* serverSimFindFreeSlot */
#include "scenario.h"
#include "control_event.h"
#include "game_sim.h"
#include "gametype.h"
#include "bases.h"
#include "pillbox.h"
#include "players.h"               /* playersIsAllie */
#include "tank.h"                  /* tankGetStats */
#include "bolo_map.h"              /* mapWrite */
#include "everard_map.h"
#include "test_harness.h"

/* Runtime fixture files (CWD-relative, like the other on-demand
 * fixture writers in this target). scenarioLoad probes for
 * "<map minus .map>.scenario.lua". */
#define SC_MAP        "ut_scen_scripted.map"
#define SC_SIDECAR    "ut_scen_scripted.scenario.lua"
#define SC_PLAIN_MAP  "ut_scen_plain.map"

/* Must match SCENARIO_MAX_ERRORS in src/server/scenario.c. */
#define SC_MAX_ERRORS 20

/* ----------------------------------------------------------------
 * Counting subscriber (same shape as test_base_win_immediate).
 * ---------------------------------------------------------------- */

typedef struct {
    int  serverTextCount;
    char lastText[512];
} ScCounter;

static void sc_count_events(void *ctx, const ControlEvent *evt) {
    ScCounter *c = (ScCounter *)ctx;
    if (evt->type == CTRL_SERVER_TEXT) {
        c->serverTextCount++;
        snprintf(c->lastText, sizeof(c->lastText), "%s",
                 evt->u.serverText.text);
    }
}

/* Registration replays current state; zero after so the counters only
 * measure what happens next. */
static SubscriberHandle sc_subscribe(ServerSim *sim, ScCounter *c) {
    SubscriberHandle h;
    memset(c, 0, sizeof(*c));
    h = serverSimRegisterSubscriber(sim, sc_count_events, c);
    memset(c, 0, sizeof(*c));
    return h;
}

/* ----------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------- */

/* Write a real BMAPBOLO .map file by booting Everard Island from its
 * compressed image and dumping it back out through mapWrite. */
static bool sc_write_map_file(const char *path) {
    BYTE emap[6000] = E_MAP;
    bool ok;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return false;
    {
        GameSim *gs = serverSimGetGameSim(sim);
        ok = (gs != NULL) &&
             mapWrite((char *)path, &gs->mp, &gs->pb, &gs->bs, &gs->ss);
    }
    serverSimDestroy(sim);
    return ok;
}

static bool sc_write_sidecar(const char *luaSource) {
    FILE *f = fopen(SC_SIDECAR, "wb");
    if (f == NULL) return false;
    fputs(luaSource, f);
    fclose(f);
    return true;
}

static void sc_cleanup_files(void) {
    remove(SC_MAP);
    remove(SC_SIDECAR);
    remove(SC_PLAIN_MAP);
}

/* Create the file-based sim (the only path that probes for sidecars). */
static ServerSim *sc_create(void) {
    return serverSimCreate((char *)SC_MAP, gameOpen, false, 0, -1);
}

/* Force the running phase, same recipe as test_base_win_immediate:
 * StartGame runs regardless of the lobby flag. */
static void sc_start_running(ServerSim *sim) {
    serverSimStartGame(sim);
}

/* ================================================================
 * 1. Sidecar discovery + metadata.
 * ================================================================ */
int run_scenario_sidecar_load(void) {
    ServerSim *sim;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_map_file(SC_PLAIN_MAP));

    /* No sidecar: plain map, no scenario, full player cap. */
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(!scenarioIsActive(sim),
                  "a map without a sidecar must not activate a scenario");
    UT_ASSERT(serverSimGetMaxPlayers(sim) == MAX_TANKS);
    serverSimDestroy(sim);

    /* Sidecar with metadata: active, max_players captured. */
    UT_ASSERT(sc_write_sidecar(
        "scenario = { name = 'UT', max_players = 3,"
        " default_brain = 'no/such/brain.lua' }\n"
        "function on_tick(game, tick) end\n"));
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioIsActive(sim),
                  "a sidecar next to the map file must boot the scenario");
    UT_ASSERT_MSG(scenarioGetMaxPlayers(sim) == 3,
                  "scenario.max_players must be captured, got %d",
                  scenarioGetMaxPlayers(sim));
    serverSimDestroy(sim);

    /* Broken sidecar: create still succeeds, scenario stays off — a
     * typo in the script must not take the whole server down. */
    UT_ASSERT(sc_write_sidecar("this is not lua ("));
    sim = sc_create();
    UT_ASSERT_MSG(sim != NULL,
                  "a sidecar that fails to compile must not fail sim create");
    UT_ASSERT_MSG(!scenarioIsActive(sim),
                  "a sidecar that fails to compile must leave the map plain");
    serverSimDestroy(sim);

    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 2. on_start fires on the first running tick, exactly once.
 * ================================================================ */
int run_scenario_on_start_first_tick(void) {
    ServerSim *sim;
    ScCounter c;
    SubscriberHandle h;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "function on_start(game) game.message('SCEN BOOT') end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scenarioIsActive(sim));
    serverSimSetLobbyEnabled(sim, false);
    sc_start_running(sim);

    /* Neither create nor StartGame may run on_start — only a tick. */
    h = sc_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "on_start ran before the first tick (%d messages)",
                  c.serverTextCount);

    serverSimTick(sim);
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "on_start must fire on the first running tick, got %d "
                  "message(s)", c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, "SCEN BOOT") != NULL,
                  "expected the script's on_start broadcast, got \"%s\"",
                  c.lastText);

    serverSimTick(sim);
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "on_start must fire exactly once per round, got %d",
                  c.serverTextCount);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 3. Round restart re-boots the VM with virgin script state.
 * ================================================================ */
int run_scenario_round_restart_fresh_state(void) {
    ServerSim *sim;
    ScCounter c;
    SubscriberHandle h;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    /* `boots` survives across rounds ONLY if the VM survives — a fresh
     * chunk run resets it to 0, so a leaky round 2 says "BOOT 2". */
    UT_ASSERT(sc_write_sidecar(
        "boots = (boots or 0) + 1\n"
        "function on_start(game) game.message('BOOT ' .. boots) end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);

    /* Round 1. */
    sc_start_running(sim);
    h = sc_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    serverSimTick(sim);
    UT_ASSERT_MSG(c.serverTextCount == 1 &&
                  strstr(c.lastText, "BOOT 1") != NULL,
                  "round 1 must boot a fresh VM (count=%d text=\"%s\")",
                  c.serverTextCount, c.lastText);

    /* Round 2 on the same map — the rotation path hops through the
     * lobby state and starts again. */
    sim->state = serverStateLobby;
    sc_start_running(sim);
    memset(&c, 0, sizeof(c));
    serverSimTick(sim);
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "round 2 must re-fire on_start (got %d messages) — a "
                  "surviving VM keeps started=true and stays silent",
                  c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, "BOOT 1") != NULL,
                  "round 2 must start from virgin globals — got \"%s\", "
                  "\"BOOT 2\" means round 1's VM state leaked through",
                  c.lastText);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 4. The player cap follows the loaded map.
 * ================================================================ */
int run_scenario_player_cap_follows_map(void) {
    ServerSim *sim;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_map_file(SC_PLAIN_MAP));
    UT_ASSERT(sc_write_sidecar(
        "scenario = { max_players = 3 }\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scenarioIsActive(sim));
    UT_ASSERT_MSG(serverSimGetMaxPlayers(sim) == 3,
                  "scenario.max_players must clamp the join cap, got %d",
                  (int)serverSimGetMaxPlayers(sim));

    /* Fill the capped slots: the join gate must refuse a 4th human. */
    serverSimAddPlayer(sim, 0, "P0", false);
    serverSimAddPlayer(sim, 1, "P1", false);
    serverSimAddPlayer(sim, 2, "P2", false);
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim) == -1,
                  "3 humans under max_players=3 must exhaust the join slots");

    /* Lobby switch to a plain map lifts the scenario and the cap. */
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT(serverSimReloadMap(sim, SC_PLAIN_MAP));
    UT_ASSERT_MSG(!scenarioIsActive(sim),
                  "switching to a plain map must drop the scenario");
    UT_ASSERT_MSG(serverSimGetMaxPlayers(sim) == MAX_TANKS,
                  "switching to a plain map must lift the scenario cap, "
                  "got %d", (int)serverSimGetMaxPlayers(sim));
    UT_ASSERT_MSG(serverSimFindFreeSlot(sim) == 3,
                  "the lifted cap must reopen slot 3");

    /* Cancelling the switch restores the scripted map AND its
     * scenario + cap (previousMapPath travels with the preview). */
    UT_ASSERT(serverSimRevertPreview(sim));
    UT_ASSERT_MSG(scenarioIsActive(sim),
                  "reverting the preview must restore the original map's "
                  "scenario");
    UT_ASSERT_MSG(serverSimGetMaxPlayers(sim) == 3,
                  "reverting the preview must restore the scenario cap, "
                  "got %d", (int)serverSimGetMaxPlayers(sim));
    UT_ASSERT(serverSimFindFreeSlot(sim) == -1);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 5. game.end_round ends the round as RETURN_REASON_SCENARIO.
 * ================================================================ */
int run_scenario_end_round_scripted_win(void) {
    ServerSim *sim;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    /* Trigger on the script's own call count, not the engine tick
     * number — the test pins the end-round plumbing, not the tick
     * cadence. */
    UT_ASSERT(sc_write_sidecar(
        "calls = 0\n"
        "function on_tick(game, tick)\n"
        "  calls = calls + 1\n"
        "  if calls >= 2 then game.end_round('Scripted win!') end\n"
        "end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    sc_start_running(sim);
    serverSimAddPlayer(sim, 0, "Holder", false);

    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round must still be running before the script's "
                  "trigger call");

    serverSimTick(sim);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "game.end_round must end the round, state=%d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SCENARIO,
                  "a scripted win must be tagged RETURN_REASON_SCENARIO "
                  "(%d), got %u — the resolver keys WBN crediting and the "
                  "lobby message off this", RETURN_REASON_SCENARIO,
                  (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(strcmp(scenarioGetWinMessage(sim), "Scripted win!") == 0,
                  "the script's text must become the win message, got "
                  "\"%s\"", scenarioGetWinMessage(sim));

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 6. Owner setters reach the engine structures.
 * ================================================================ */
int run_scenario_owner_setters(void) {
    ServerSim *sim;
    GameSim *gs;
    BYTE baseOwnerBefore, pillOwnerBefore;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "function on_start(game)\n"
        "  game.set_base_owner(1, 0)\n"
        "  game.set_pill_owner(1, 0)\n"
        "end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) > 0);
    UT_ASSERT(pillsGetNumPills(&gs->pb) > 0);

    sc_start_running(sim);
    /* Ownership on Everard starts neutral; the player must exist for
     * the setters to make sense to observers. */
    serverSimAddPlayer(sim, 0, "Owner", false);
    baseOwnerBefore = (*gs->bs).item[0].owner;
    pillOwnerBefore = (*gs->pb).item[0].owner;
    UT_ASSERT(baseOwnerBefore != 0);
    UT_ASSERT(pillOwnerBefore != 0);

    serverSimTick(sim);   /* fires on_start */
    UT_ASSERT_MSG((*gs->bs).item[0].owner == 0,
                  "game.set_base_owner(1, 0) must set base 1's engine "
                  "owner to player 0, got %u",
                  (unsigned)(*gs->bs).item[0].owner);
    UT_ASSERT_MSG((*gs->pb).item[0].owner == 0,
                  "game.set_pill_owner(1, 0) must set pill 1's engine "
                  "owner to player 0, got %u",
                  (unsigned)(*gs->pb).item[0].owner);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 8. give_pill: 1-based, exactly-once, engine state is the proof.
 * ================================================================ */
int run_scenario_give_pill(void) {
    ServerSim *sim;
    GameSim *gs;
    ScCounter c;
    SubscriberHandle h;
    BYTE numPills;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    /* LAST must exercise the top of the 1-based range: the pills* API
     * decrements internally, so an off-by-one here either no-ops
     * (0-based bug: 'LAST' misses the final pill) or rejects. */
    UT_ASSERT(sc_write_sidecar(
        "function on_start(game)\n"
        "  local n = game.num_pills()\n"
        "  local ok1 = game.give_pill(0, 1)\n"
        "  local okN = game.give_pill(0, n)\n"
        "  local again = game.give_pill(0, 1)\n"        /* already carried */
        "  local bad = game.give_pill(0, n + 1)\n"      /* out of range */
        "  if ok1 and okN and (not again) and (not bad) then\n"
        "    game.message('GIVE OK')\n"
        "  end\n"
        "end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    numPills = pillsGetNumPills(&gs->pb);
    UT_ASSERT(numPills >= 2);
    sc_start_running(sim);
    serverSimAddPlayer(sim, 0, "Carrier", false);

    h = sc_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    serverSimTick(sim);   /* fires on_start */

    UT_ASSERT_MSG(strstr(c.lastText, "GIVE OK") != NULL,
                  "give_pill acceptance/rejection contract broke "
                  "(script did not report GIVE OK; last=\"%s\")",
                  c.lastText);
    UT_ASSERT_MSG((*gs->pb).item[0].inTank == TRUE,
                  "pill 1 must be in the tank after give_pill(0, 1)");
    UT_ASSERT_MSG((*gs->pb).item[numPills - 1].inTank == TRUE,
                  "pill %u must be in the tank after give_pill(0, num_pills) "
                  "— a 0-based slip makes the LAST pill unloadable",
                  (unsigned)numPills);
    UT_ASSERT_MSG((*gs->pb).item[0].owner == 0 &&
                  (*gs->pb).item[numPills - 1].owner == 0,
                  "given pills must transfer ownership to the carrier");
    UT_ASSERT_MSG((*gs->pb).item[1].inTank == FALSE,
                  "pills never given must stay on the map");

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 9. scenario.game beats the lobby's game type, every round.
 * ================================================================ */
int run_scenario_forced_game_type(void) {
    ServerSim *sim;
    GameSim *gs;
    BYTE shells, mines, armour, trees;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "scenario = { game = 'tournament' }\n"
        "function on_tick(game, tick) end\n"));

    /* The sim is created as OPEN — the sidecar must flip it. */
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gameTypeGet(&gs->game) == gameTournament,
                  "scenario.game must force the game type at create, got %d",
                  (int)gameTypeGet(&gs->game));

    /* A lobby could still flip it back before the round starts — the
     * round-start scenario reboot must re-force it BEFORE the round's
     * tanks are created. */
    gameTypeSet(&gs->game, gameOpen);
    sc_start_running(sim);
    UT_ASSERT_MSG(gameTypeGet(&gs->game) == gameTournament,
                  "round start must re-force scenario.game over the lobby "
                  "setting, got %d", (int)gameTypeGet(&gs->game));

    /* And the tanks really spawn under it: the loadout must be the
     * TOURNAMENT one (scaled by the map's neutral-base fraction), not
     * the open full tank the sim was created as. */
    serverSimAddPlayer(sim, 0, "Farmer", false);
    UT_ASSERT(gs->tanks[0] != NULL);
    tankGetStats(&gs->tanks[0], &shells, &mines, &armour, &trees);
    {
        BYTE expShells, expMines, expArmour, expTrees;
        gameType g = gameTournament;
        gameTypeGetItems(gs, &g, &expShells, &expMines, &expArmour,
                         &expTrees);
        UT_ASSERT_MSG(shells == expShells && mines == expMines &&
                      trees == expTrees,
                      "a tank spawned under forced tournament rules must "
                      "carry the tournament loadout (%u/%u/%u), got "
                      "%u/%u/%u", (unsigned)expShells, (unsigned)expMines,
                      (unsigned)expTrees, (unsigned)shells,
                      (unsigned)mines, (unsigned)trees);
    }

    /* The per-slot spawn-loadout override (spawn_bot's mode, and every
     * respawn after it) beats the sim rules: slot 1 armed as OPEN in
     * this tournament sim must come out with the full tank. */
    gs->spawnLoadout[1] = (BYTE)gameOpen;
    serverSimAddPlayer(sim, 1, "Raider", false);
    UT_ASSERT(gs->tanks[1] != NULL);
    tankGetStats(&gs->tanks[1], &shells, &mines, &armour, &trees);
    {
        BYTE expShells, expMines, expArmour, expTrees;
        gameType g = gameOpen;
        gameTypeGetItems(gs, &g, &expShells, &expMines, &expArmour,
                         &expTrees);
        UT_ASSERT_MSG(shells == expShells && mines == expMines,
                      "a slot with an open spawn-loadout override must "
                      "spawn with the full tank (%u/%u), got %u/%u",
                      (unsigned)expShells, (unsigned)expMines,
                      (unsigned)shells, (unsigned)mines);
    }

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 10. on_choose_start forces the placement start; bad answers fall
 *     through to the engine pick.
 * ================================================================ */
int run_scenario_choose_start(void) {
    ServerSim *sim;
    GameSim *gs;
    start s1;
    WORLD wx, wy;
    BYTE mx, my;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));

    /* Pin player 0 to start 1. The hook fires DURING tank creation, so
     * no tick is needed — and this also proves it works before
     * on_start (round start places tanks before the first tick). */
    UT_ASSERT(sc_write_sidecar(
        "function on_choose_start(game, p)\n"
        "  if p == 0 then return 1 end\n"
        "  return nil\n"
        "end\n"
        "function on_tick(game, tick) end\n"));
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(startsGetNumStarts(&gs->ss) >= 1);
    startsGetStartStruct(&gs->ss, &s1, 1);
    sc_start_running(sim);
    serverSimAddPlayer(sim, 0, "Pinned", false);
    UT_ASSERT(gs->tanks[0] != NULL);
    tankGetWorld(&gs->tanks[0], &wx, &wy);
    mx = (BYTE)(wx >> TANK_SHIFT_MAPSIZE);
    my = (BYTE)(wy >> TANK_SHIFT_MAPSIZE);
    UT_ASSERT_MSG(abs((int)mx - (int)s1.x) <= 6 &&
                  abs((int)my - (int)s1.y) <= 6,
                  "on_choose_start returning 1 must place the tank at "
                  "start 1 (%u,%u) +/- scatter, got (%u,%u)",
                  (unsigned)s1.x, (unsigned)s1.y,
                  (unsigned)mx, (unsigned)my);
    serverSimDestroy(sim);

    /* An out-of-range answer must fall through, not wedge placement. */
    UT_ASSERT(sc_write_sidecar(
        "function on_choose_start(game, p) return 99 end\n"
        "function on_tick(game, tick) end\n"));
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    sc_start_running(sim);
    serverSimAddPlayer(sim, 0, "Fallthrough", false);
    UT_ASSERT_MSG(gs->tanks[0] != NULL,
                  "a bad on_choose_start answer must still leave the "
                  "engine placing the tank");

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 11. on_setup: a silent pre-snapshot tick — mutations land before
 *     the round runs, and their delta events are flushed.
 * ================================================================ */
int run_scenario_setup_pre_snapshot(void) {
    ServerSim *sim;
    GameSim *gs;
    ScCounter c;
    SubscriberHandle h;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "function on_setup(game) game.set_base_owner(1, 0) end\n"
        "function on_start(game) game.message('STARTED') end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT((*gs->bs).item[0].owner != 0);
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "Arranger", false);

    sc_start_running(sim);
    UT_ASSERT_MSG((*gs->bs).item[0].owner == 0,
                  "on_setup must run inside StartGame, before the first "
                  "tick — base 1 still owned by %u",
                  (unsigned)(*gs->bs).item[0].owner);
    UT_ASSERT_MSG(sim->eventCount == 0 && sim->mapEventCount == 0,
                  "setup's world-delta events must be flushed so clients "
                  "meet the arranged world as the baseline (evt=%d map=%d)",
                  sim->eventCount, sim->mapEventCount);

    /* on_start still belongs to the first running tick — after setup. */
    h = sc_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT(c.serverTextCount == 0);
    serverSimTick(sim);
    UT_ASSERT_MSG(c.serverTextCount == 1 &&
                  strstr(c.lastText, "STARTED") != NULL,
                  "on_start must fire once on the first tick after setup "
                  "(count=%d text=\"%s\")", c.serverTextCount, c.lastText);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 12. The scenario's lobby-behavior surface is METHODS the engine
 *     calls: enemy_bots(game) sizes the Team 2 seed, and
 *     show_add_team_button(game) gates Add Team (default: hidden on
 *     scenario maps, shown on plain maps). Also pins the
 *     scenario.description pipeline end-to-end (a full-length blurb
 *     survives untruncated into the lobby-settings event).
 * ================================================================ */
int run_scenario_enemy_roster(void) {
    ServerSim *sim;
    ControlEvent evt;
    /* Long enough to catch a repeat of the 160-byte truncation. */
    static const char *kDesc =
        "Co-op survival: hold the island's center through wave after wave "
        "of AI attackers storming in from the outer ring. Hold your bases, "
        "grab the dead pillboxes, fort up in the forest - win by keeping "
        "the enemy from taking over the center.";

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_map_file(SC_PLAIN_MAP));
    {
        char sidecar[1024];
        snprintf(sidecar, sizeof(sidecar),
                 "scenario = { max_players = 6,\n"
                 "  description = \"%s\" }\n"
                 "function enemy_bots(game) return 10 end\n"
                 "function on_tick(game, tick) end\n", kDesc);
        UT_ASSERT(sc_write_sidecar(sidecar));
    }

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioGetEnemyBots(sim) == 10,
                  "the enemy_bots(game) hook must size the Team 2 seed at "
                  "10, got %d", scenarioGetEnemyBots(sim));
    UT_ASSERT_MSG(scenarioGetAllowExtraTeams(sim) == FALSE,
                  "scenario maps default to two sides — Add Team hidden");
    UT_ASSERT_MSG(strcmp(scenarioGetDescription(sim), kDesc) == 0,
                  "scenario.description must survive untruncated (%zu "
                  "chars), got %zu", strlen(kDesc),
                  strlen(scenarioGetDescription(sim)));

    /* The lobby-settings event carries the answers to every client. */
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT(evt.u.lobbySettings.lobbyScenarioMap == true);
    UT_ASSERT(evt.u.lobbySettings.lobbyScenarioExtraTeams == false);
    UT_ASSERT(strcmp(evt.u.lobbySettings.lobbyScenarioDesc, kDesc) == 0);

    /* The seeding pass runs on the map-change funnel; with no bot brain
     * configured in this harness it must decline gracefully — no bots,
     * no crash. */
    serverSimSeedScenarioEnemyTeam(sim);
    UT_ASSERT(serverSimGetNumBots(sim) == 0);
    serverSimDestroy(sim);

    /* A script that answers the hooks differently is obeyed: Add Team
     * shown, no enemy seed wanted. */
    UT_ASSERT(sc_write_sidecar(
        "function show_add_team_button(game) return true end\n"
        "function on_tick(game, tick) end\n"));
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scenarioGetEnemyBots(sim) == 0);
    UT_ASSERT_MSG(scenarioGetAllowExtraTeams(sim) == TRUE,
                  "show_add_team_button() returning true must be obeyed");

    /* Plain maps: no scenario — Add Team always available. */
    UT_ASSERT(serverSimReloadMap(sim, SC_PLAIN_MAP));
    UT_ASSERT(!scenarioIsActive(sim));
    UT_ASSERT(scenarioGetAllowExtraTeams(sim) == TRUE);
    UT_ASSERT(scenarioGetEnemyBots(sim) == 0);
    serverSimDestroy(sim);

    /* The invariant "all human players are represented": a hostile
     * script hammering every removal API at a HUMAN slot achieves
     * nothing — the guards refuse and the post-hook audit stands
     * ready behind them. */
    UT_ASSERT(sc_write_sidecar(
        "function on_setup(game)\n"
        "  game.remove_bot(0)\n"
        "  game.lobby_remove_bot(0)\n"
        "end\n"
        "function on_tick(game, tick) end\n"));
    sim = sc_create();
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    serverSimAddPlayer(sim, 0, "Human", false);
    sc_start_running(sim);
    serverSimTick(sim);   /* lazy setup path runs the hostile on_setup */
    UT_ASSERT_MSG(serverSimIsPlayerConnected(sim, 0),
                  "a scenario must NEVER be able to remove a human "
                  "player — slot 0 was disconnected");
    serverSimDestroy(sim);

    /* Invalid team setups from the script: the API refuses them, and
     * the post-hook audit clamps any invalid team that appears through
     * a path the API doesn't cover. */
    {
        ScCounter c;
        SubscriberHandle h;
        UT_ASSERT(sc_write_sidecar(
            "function on_lobby(game)\n"
            "  local ok0 = game.lobby_set_team(0, 0)\n"
            "  local ok99 = game.lobby_set_team(0, 99)\n"
            "  if not ok0 and not ok99 then game.message('REFUSED') end\n"
            "end\n"
            "function on_tick(game, tick) end\n"));
        sim = sc_create();          /* lobby state — on_lobby territory */
        UT_ASSERT(sim != NULL);
        serverSimAddPlayer(sim, 0, "Human", false);
        sim->lobbyPlayers[0].teamNumber = 5;
        h = sc_subscribe(sim, &c);
        UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

        scenarioLobbyChanged(sim);
        UT_ASSERT_MSG(strstr(c.lastText, "REFUSED") != NULL,
                      "lobby_set_team must refuse teams 0 and 99 "
                      "(last=\"%s\")", c.lastText);
        UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 5,
                      "a refused team change must leave the slot's team "
                      "untouched, got %u",
                      (unsigned)sim->lobbyPlayers[0].teamNumber);

        /* An invalid team arriving via a non-API path: the audit
         * clamps it back to team 1 after the next hook run. */
        sim->lobbyPlayers[0].teamNumber = 0;
        scenarioLobbyChanged(sim);
        UT_ASSERT_MSG(sim->lobbyPlayers[0].teamNumber == 1,
                      "the roster audit must clamp an invalid team to 1, "
                      "got %u", (unsigned)sim->lobbyPlayers[0].teamNumber);
        serverSimDestroy(sim);
    }

    /* game.set_team(p, N) must never ally slot N's OCCUPANT with team N
     * — the scenario's teamOf defaults used to be the slot index, so
     * putting wave bots on team 2 silently allied whoever sat in SEAT 2
     * with the whole enemy team (and the wave's pills migrated to the
     * defenders when it vanished). Real same-team pairs still ally. */
    {
        GameSim *gs;
        UT_ASSERT(sc_write_sidecar(
            "function on_start(game)\n"
            "  game.set_team(6, 2)\n"
            "  game.set_team(7, 2)\n"
            "end\n"
            "function on_tick(game, tick) end\n"));
        sim = sc_create();
        UT_ASSERT(sim != NULL);
        gs = serverSimGetGameSim(sim);
        serverSimSetLobbyEnabled(sim, false);
        serverSimAddPlayer(sim, 2, "SeatTwo", false);
        serverSimAddPlayer(sim, 6, "Wave-A", false);
        serverSimAddPlayer(sim, 7, "Wave-B", false);
        /* Distinct lobby teams so the engine's own team-alliance
         * reapply at start allies none of them — the only pairing
         * left comes from the scenario's set_team path under test. */
        sim->lobbyPlayers[2].teamNumber = 3;
        sim->lobbyPlayers[6].teamNumber = 8;
        sim->lobbyPlayers[7].teamNumber = 9;
        sc_start_running(sim);
        serverSimTick(sim);   /* fires on_start's set_team calls */
        UT_ASSERT_MSG(!playersIsAllie(&gs->plyrs, 2, 6) &&
                      !playersIsAllie(&gs->plyrs, 2, 7),
                      "seat 2's occupant must NOT be allied with team 2");
        UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, 6, 7),
                      "slots on the same scenario team must be allied");
        serverSimDestroy(sim);
    }

    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 13. Full seeded round start — the path the lobby's Run button takes:
 *     a scenario lobby with REAL seeded enemy bots (GoalHunter, loaded
 *     from the build dir) goes through serverSimStartGame, where
 *     on_setup captures and removes the roster mid-start. Reproduces
 *     the in-game flow end to end; must not crash and must leave the
 *     human standing with the enemy roster cleared.
 * ================================================================ */
int run_scenario_seeded_round_start(void) {
    ServerSim *sim;
    int i, bots;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "scenario = { max_players = 6,\n"
        "  default_brain = 'Brains/GoalHunter_1.7/init.lua' }\n"
        "function enemy_bots(game) return 10 end\n"
        "names = {}\n"
        "function on_setup(game)\n"
        "  for p = 0, 15 do\n"
        "    local ls = game.lobby_slot(p)\n"
        "    if ls ~= nil and ls.bot and ls.team == 2 then\n"
        "      names[#names + 1] = ls.name\n"
        "      game.remove_bot(p)\n"
        "    end\n"
        "  end\n"
        "end\n"
        "function on_choose_start(game, p)\n"
        "  local ls = game.lobby_slot(p)\n"
        "  if ls ~= nil and ls.team == 2 then return 7 end\n"
        "  return 1\n"
        "end\n"
        "function on_start(game)\n"
        "  game.message('CAPTURED ' .. #names)\n"
        "end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scenarioIsActive(sim));

    /* Commit = the lobby takeover: seeds 10 REAL brained bots. If the
     * brain fails to load in this environment the seed declines — then
     * there is nothing to reproduce here, so pass trivially (the live
     * BrainTest runs cover that half). */
    serverSimApplyScenarioCommit(sim);
    bots = (int)serverSimGetNumBots(sim);
    if (bots == 0) {
        fprintf(stderr, "  (no brain loadable here — seeded-start repro "
                        "skipped)\n");
        serverSimDestroy(sim);
        sc_cleanup_files();
        return 0;
    }
    UT_ASSERT_MSG(bots == 10, "seed must field 10 bots, got %d", bots);

    serverSimAddPlayer(sim, 0, "Human", false);

    /* The Run click. */
    serverSimStartGame(sim);

    UT_ASSERT_MSG(serverSimIsPlayerConnected(sim, 0),
                  "the human must survive the seeded round start");
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "on_setup must have pulled all seeded bots off the "
                  "field, %d remain", (int)serverSimGetNumBots(sim));

    for (i = 0; i < 5; i++) serverSimTick(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 6c. Last human leaving the lobby must not strip the scenario.
 * ================================================================ */
int run_scenario_lobby_reset_recommit(void) {
    ServerSim *sim;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    UT_ASSERT(sc_write_sidecar(
        "function enemy_bots(game) return 4 end\n"
        "function on_tick(game, tick) end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scenarioIsActive(sim));

    /* The operator's startup snapshot predates the scenario: the server
     * booted on plain Open settings before Choose Map picked this map. */
    sim->originalLobbySettings.valid    = true;
    sim->originalLobbySettings.gameType = gameOpen;

    /* Choose Map commit — scenario takes the lobby over. */
    serverSimApplyScenarioCommit(sim);
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "the commit must force the Scenario game type, got %d",
                  (int)serverSimGetGameType(sim));

    /* A human joins the lobby and leaves again. The last-human-out wipe
     * restores the startup defaults (Open) — the scenario must re-commit
     * itself on top, or the next joiner finds a plain lobby with no
     * enemy team (the leave-and-rejoin bug). */
    serverSimAddPlayer(sim, 0, "Human", false);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    serverSimRemovePlayer(sim, 0);

    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "the empty-lobby reset must re-commit the scenario map's "
                  "game type, got %d", (int)serverSimGetGameType(sim));

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

/* ================================================================
 * 7. A persistently-erroring script disables itself, sim survives.
 * ================================================================ */
int run_scenario_error_disables_after_limit(void) {
    ServerSim *sim;
    ScCounter c;
    SubscriberHandle h;
    int i;

    sc_cleanup_files();
    UT_ASSERT(sc_write_map_file(SC_MAP));
    /* Every on_tick broadcasts, then throws. The broadcast lands before
     * the error, so the message count IS the number of ticks that still
     * reached the script — it must stop at SCENARIO_MAX_ERRORS. */
    UT_ASSERT(sc_write_sidecar(
        "function on_tick(game, tick)\n"
        "  game.message('T')\n"
        "  error('boom')\n"
        "end\n"));

    sim = sc_create();
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    sc_start_running(sim);
    h = sc_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    for (i = 0; i < SC_MAX_ERRORS + 10; i++) {
        serverSimTick(sim);
    }

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "an erroring script must never take the round down, "
                  "state=%d", (int)serverSimGetState(sim));
    /* SC_MAX_ERRORS script broadcasts, then the engine's own "scenario
     * disabled" notice — and nothing after that. */
    UT_ASSERT_MSG(c.serverTextCount == SC_MAX_ERRORS + 1,
                  "the scenario must disable itself after exactly %d "
                  "consecutive errors (+1 disable notice) — saw %d "
                  "messages", SC_MAX_ERRORS, c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, "disabled") != NULL,
                  "the last broadcast must be the disable notice so "
                  "players learn the script died, got \"%s\"", c.lastText);

    serverSimDestroy(sim);
    sc_cleanup_files();
    return 0;
}

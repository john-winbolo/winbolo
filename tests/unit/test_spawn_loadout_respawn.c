/*
 * Per-slot spawn-loadout override across RESPAWNS
 * (GameSim::spawnLoadout[], src/bolo/tank.c).
 *
 * game.spawn_bot(name, brain, team, "open") in a scripted scenario
 * (src/server/scenario.c:l_spawn_bot) arms
 *   serverSimGetGameSim(sim)->spawnLoadout[slot] = gameOpen
 * BEFORE the bot's tank exists, and the slot's player-removal path
 * (src/server/server_sim.c serverSimRemovePlayer) zeroes it again.
 *
 * The claim under test — the one a survival map depends on — is that a
 * wave bot armed this way comes back FULLY ARMED on EVERY respawn while
 * the humans sharing the sim keep playing under the sim's own rules.
 *
 * Two distinct engine paths have to honour the override for that to be
 * true, and they are NOT the same code:
 *
 *   FIRST spawn  -> tankCreate()  (tank.c) — reads sim->spawnLoadout[plr]
 *   EVERY respawn-> tankDeath()   (tank.c) — the server-authoritative
 *                                  death/reposition/refuel routine that
 *                                  tankUpdate calls when deathWait hits 0
 *
 * So the tests below drive the REAL respawn: spend the tank's stock,
 * mark it dead (armour > TANK_FULL_ARMOUR), run the death timer down
 * through serverSimTick, and re-read the stats the engine handed back.
 * Calling tankCreate a second time would prove nothing about respawns —
 * nothing in the running server calls it after the join.
 *
 * Expected numbers are never hardcoded: they come from gameTypeGetItems
 * for the game type in question, evaluated at compare time (the
 * tournament loadout tracks the map's live neutral-base fraction).
 *
 * No fixture files: the sim is booted straight from the compressed
 * Everard Island image, created as TOURNAMENT so the open override is
 * unmistakably different from the sim's own rules.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state poke */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "game_sim.h"
#include "gametype.h"
#include "tank.h"                  /* tankGetStats / tankSetStats / deathWait */
#include "everard_map.h"
#include "test_harness.h"

/* Everard Island, as a tournament sim: humans farm, so the tournament
 * loadout is provably not the full tank. */
static ServerSim *sl_create_running(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameTournament, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);        /* running: addPlayer creates tanks now */
    return sim;
}

/* What gameTypeGetItems says a tank of `g` should hold RIGHT NOW. */
static void sl_expected(GameSim *gs, gameType g,
                        BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
    gameType t = g;
    gameTypeGetItems(gs, &t, shells, mines, armour, trees);
}

/* Drive the real server respawn for slot p: strip the tank bare, kill
 * it, and let the death timer expire inside serverSimTick — the same
 * sequence tankUpdate runs when a shell finishes a tank off.
 * Returns false if the engine never brought the tank back. */
static bool sl_respawn(ServerSim *sim, BYTE p) {
    GameSim *gs = serverSimGetGameSim(sim);
    int guard;
    if (gs == NULL || gs->tanks[p] == NULL) return false;

    /* Empty the tank so a respawn that silently skips the refuel is
     * visible as 0/0/0 instead of leftover stock. */
    tankSetStats(&gs->tanks[p], 0, 0, (BYTE)(TANK_FULL_ARMOUR + 1), 0);
    tankSetDeathWait(&gs->tanks[p], 2);

    for (guard = 0; guard < 64; guard++) {
        serverSimTick(sim);
        if (gs->tanks[p] == NULL) return false;
        if (tankGetArmour(&gs->tanks[p]) <= TANK_FULL_ARMOUR &&
            tankGetDeathWait(&gs->tanks[p]) == 0) {
            return true;
        }
    }
    return false;
}

/* ================================================================
 * A + B: an open-mode override arms the slot for the first spawn AND
 *        for every respawn after it.
 * ================================================================ */
int run_spawn_loadout_open_every_respawn(void) {
    ServerSim *sim;
    GameSim *gs;
    BYTE shells, mines, armour, trees;
    BYTE eShells, eMines, eArmour, eTrees;
    const BYTE SLOT = 15;          /* spawn_bot fills from the top down */
    int life;

    sim = sl_create_running();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(gameTypeGet(&gs->game) == gameTournament,
                  "fixture must run tournament rules, got %d",
                  (int)gameTypeGet(&gs->game));

    /* The open loadout must actually differ from this sim's own rules,
     * or the test proves nothing. */
    sl_expected(gs, gameOpen,        &eShells, &eMines, &eArmour, &eTrees);
    sl_expected(gs, gameTournament,  &shells,  &mines,  &armour,  &trees);
    UT_ASSERT_MSG(eShells != shells || eMines != mines || eTrees != trees,
                  "fixture is degenerate: open (%u/%u/%u) and tournament "
                  "(%u/%u/%u) loadouts are identical",
                  (unsigned)eShells, (unsigned)eMines, (unsigned)eTrees,
                  (unsigned)shells, (unsigned)mines, (unsigned)trees);

    /* --- Case A: arm the slot, then let the join create the tank. --- */
    gs->spawnLoadout[SLOT] = (BYTE)gameOpen;
    serverSimAddPlayer(sim, SLOT, "WaveBot", false);
    UT_ASSERT_MSG(gs->tanks[SLOT] != NULL,
                  "slot %u got no tank on join", (unsigned)SLOT);

    tankGetStats(&gs->tanks[SLOT], &shells, &mines, &armour, &trees);
    sl_expected(gs, gameOpen, &eShells, &eMines, &eArmour, &eTrees);
    UT_ASSERT_MSG(shells == eShells && mines == eMines &&
                  armour == eArmour && trees == eTrees,
                  "FIRST spawn under an open override must be the full "
                  "open loadout %u/%u/%u/%u, got %u/%u/%u/%u",
                  (unsigned)eShells, (unsigned)eMines, (unsigned)eArmour,
                  (unsigned)eTrees, (unsigned)shells, (unsigned)mines,
                  (unsigned)armour, (unsigned)trees);

    /* --- Case B: EVERY respawn, not just the first spawn. --- */
    for (life = 2; life <= 4; life++) {
        UT_ASSERT_MSG(sl_respawn(sim, SLOT),
                      "engine never respawned slot %u for life %d",
                      (unsigned)SLOT, life);

        tankGetStats(&gs->tanks[SLOT], &shells, &mines, &armour, &trees);
        sl_expected(gs, gameOpen, &eShells, &eMines, &eArmour, &eTrees);
        UT_ASSERT_MSG(shells == eShells && mines == eMines &&
                      armour == eArmour && trees == eTrees,
                      "RESPAWN #%d under an open override must be the full "
                      "open loadout %u/%u/%u/%u, got %u/%u/%u/%u — the "
                      "override is honoured by tankCreate but dropped by "
                      "the death/respawn path (tankDeath)",
                      life - 1,
                      (unsigned)eShells, (unsigned)eMines, (unsigned)eArmour,
                      (unsigned)eTrees, (unsigned)shells, (unsigned)mines,
                      (unsigned)armour, (unsigned)trees);
    }

    /* The override itself must survive all of it. */
    UT_ASSERT_MSG(gs->spawnLoadout[SLOT] == (BYTE)gameOpen,
                  "the slot's override must persist across respawns, got %u",
                  (unsigned)gs->spawnLoadout[SLOT]);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * C + D: an unarmed slot follows the sim rules, and the leave-path
 *        clear stops the override leaking to the next occupant.
 * ================================================================ */
int run_spawn_loadout_human_unaffected(void) {
    ServerSim *sim;
    GameSim *gs;
    BYTE shells, mines, armour, trees;
    BYTE eShells, eMines, eArmour, eTrees;
    const BYTE HUMAN = 0;
    const BYTE SLOT  = 15;

    sim = sl_create_running();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    /* --- Case C: no override -> the sim's tournament rules, on the
     *     first spawn and on every respawn. --- */
    UT_ASSERT_MSG(gs->spawnLoadout[HUMAN] == 0,
                  "a fresh sim must have no spawn-loadout override, got %u",
                  (unsigned)gs->spawnLoadout[HUMAN]);
    serverSimAddPlayer(sim, HUMAN, "Farmer", false);
    UT_ASSERT(gs->tanks[HUMAN] != NULL);

    tankGetStats(&gs->tanks[HUMAN], &shells, &mines, &armour, &trees);
    sl_expected(gs, gameTournament, &eShells, &eMines, &eArmour, &eTrees);
    UT_ASSERT_MSG(shells == eShells && mines == eMines &&
                  armour == eArmour && trees == eTrees,
                  "an unarmed slot must spawn under the sim rules "
                  "%u/%u/%u/%u, got %u/%u/%u/%u",
                  (unsigned)eShells, (unsigned)eMines, (unsigned)eArmour,
                  (unsigned)eTrees, (unsigned)shells, (unsigned)mines,
                  (unsigned)armour, (unsigned)trees);

    UT_ASSERT_MSG(sl_respawn(sim, HUMAN), "engine never respawned the human");
    tankGetStats(&gs->tanks[HUMAN], &shells, &mines, &armour, &trees);
    sl_expected(gs, gameTournament, &eShells, &eMines, &eArmour, &eTrees);
    UT_ASSERT_MSG(shells == eShells && mines == eMines &&
                  armour == eArmour && trees == eTrees,
                  "an unarmed slot must RESPAWN under the sim rules "
                  "%u/%u/%u/%u, got %u/%u/%u/%u",
                  (unsigned)eShells, (unsigned)eMines, (unsigned)eArmour,
                  (unsigned)eTrees, (unsigned)shells, (unsigned)mines,
                  (unsigned)armour, (unsigned)trees);

    /* --- Case D: the wave bot leaves; the next occupant of its slot
     *     must NOT inherit the open loadout. --- */
    gs->spawnLoadout[SLOT] = (BYTE)gameOpen;
    serverSimAddPlayer(sim, SLOT, "WaveBot", false);
    UT_ASSERT(gs->tanks[SLOT] != NULL);

    serverSimRemovePlayer(sim, SLOT);
    UT_ASSERT_MSG(gs->spawnLoadout[SLOT] == 0,
                  "removing the slot's player must clear its spawn-loadout "
                  "override, got %u", (unsigned)gs->spawnLoadout[SLOT]);

    serverSimAddPlayer(sim, SLOT, "LateHuman", false);
    UT_ASSERT(gs->tanks[SLOT] != NULL);
    tankGetStats(&gs->tanks[SLOT], &shells, &mines, &armour, &trees);
    sl_expected(gs, gameTournament, &eShells, &eMines, &eArmour, &eTrees);
    UT_ASSERT_MSG(shells == eShells && mines == eMines &&
                  armour == eArmour && trees == eTrees,
                  "the next occupant of a cleared slot must spawn under the "
                  "sim rules %u/%u/%u/%u, got %u/%u/%u/%u — the wave bot's "
                  "open loadout leaked",
                  (unsigned)eShells, (unsigned)eMines, (unsigned)eArmour,
                  (unsigned)eTrees, (unsigned)shells, (unsigned)mines,
                  (unsigned)armour, (unsigned)trees);

    serverSimDestroy(sim);
    return 0;
}

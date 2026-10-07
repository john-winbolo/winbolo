/*
 * A mod's start pick and the lobby's (test_mod_lobby_starts.c).
 *
 * Players reported that picking starts on opposite sides in the lobby did
 * nothing: both spawned on the same side, as if the starts were random. The
 * Mac Bolo Rules mod answers on_choose_start with a random clear start, and
 * the engine asks that policy ahead of the lobby's batch slot, so with the
 * mod on, the round's opening tanks ignored every lobby pick and every team
 * side. The engine now tells the policy the lobby's start and the mod hands
 * it back for an opening tank; the mod's pick is for the spawns after it.
 *
 *   run_mod_lobby_starts_mac_bolo_keeps_opposite_sides
 *       two humans on teams 1 and 2, sides west and east, each holding a
 *       start picked by hand on their own side: with Mac Bolo Rules on,
 *       every round opens with each tank on its own pick, round after
 *       round, and once the round is running the mod still names the
 *       start for a later spawn.
 *
 * The mod's lobby settings: one per part, each on by default, and
 * first_start_lobby ("Override first start to what was chosen in lobby").
 *
 *   run_mac_bolo_settings_defaults_play_old_rules
 *       nothing chosen: the round plays the eleven rules the mod's written
 *       table always set, at the same numbers.
 *   run_mac_bolo_settings_each_part_off
 *       each part turned off on its own: that part's rules are back on the
 *       classic table and every other part's rules are unchanged.
 *   run_mac_bolo_settings_numbers
 *       the five number settings reach their rules.
 *   run_mac_bolo_first_start_lobby_off_uses_mac_pick
 *       first_start_lobby off: across several rounds some opening tank
 *       stands away from its lobby pick, where the Mac Bolo pick put it,
 *       and the mod still names the start for a later spawn.
 *   run_mac_bolo_spawn_starts_off_engine_picks
 *       spawn_starts off: the opening tanks are on their lobby picks and the
 *       mod names no start for a spawn, so the engine picks.
 *   run_mac_bolo_setting_changed_after_attach
 *       the lobby's own order: the mod attached on its defaults, then the
 *       host turns shell_cap off in the details dialog. The round plays the
 *       classic shell cap rules, so the round reads the host's picks, not
 *       the ones the mod was attached with.
 *   run_mac_bolo_setting_changed_between_rounds
 *       round 1 on the defaults, back to the lobby, shell_cap off and
 *       push_step changed: round 2 plays the new picks.
 *
 * on_choose_start(p, lobby): the engine hands each script the start the
 * lobby reserved for the tank, and game.lobby_start / game.lobby_side read
 * the lobby beside it. These use a small mod written for the case.
 *
 *   run_choose_start_lobby_arg_hand_picked
 *       each seat is told its hand pick, game.lobby_start agrees and
 *       game.lobby_side names the team's side; once the opening tanks are
 *       built every seat reads nil, and a respawn is told nil.
 *   run_choose_start_lobby_arg_team_side
 *       no hand pick: each seat is told the start the lobby showed for it,
 *       on its team's side.
 *   run_choose_start_lobby_arg_nil_without_reservation
 *       a seat joining a running round is told nil.
 *   run_choose_start_override_releases_reservation
 *       a script naming a third start wins; the seat's reservation is spent
 *       and its respawn is told nil; the other seat keeps its own.
 *   run_choose_start_override_onto_other_reservation
 *       a script sending seat 0 to seat 1's reserved start: both tanks open
 *       by it on squares of their own, and no reservation is left over.
 *   run_choose_start_script_ignoring_lobby_unchanged
 *       a one-parameter policy is answered as it always was.
 *
 * The mod is the shipped file, read from the data/mods directory the build
 * stages beside the test binary, attached through scenarioHostAttachMod as
 * the lobby's Mods setting attaches it. A setting is chosen through
 * serverSimSetScriptSetting, which reads the declaration from the scenario
 * directory, here set to that same data/mods directory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"              /* gameSimChooseStart */
#include "client_command.h"        /* CMD_LOBBY_TEAM_META, CMD_LOBBY_CLAIM_START */
#include "everard_map.h"
#include "starts.h"                /* startsGetNumStarts */
#include "start_sides.h"           /* START_SIDE_*, START_SIDE_BIT_* */
#include "tank.h"                  /* tankGetWorld */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, sim->lobbyPlayers */
#include "server_sim_lifecycle.h"  /* serverSimApplyInstanceConfig, StartGame,
                                    * serverSimReturnToLobby */
#include "server_sim_join.h"       /* serverSimLobbyStartSideMask */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "scenario_host.h"         /* scenarioHostAttachMod,
                                    * scenarioHostRegisterScenarioLister */
#include "sim_rules.h"             /* SimRules, simRulesClassic */
#include "threads.h"
#include "test_harness.h"

#define MLS_MOD_FILE "MacBoloRules.scenario.lua"
#define MLS_ROUNDS   6

static CmdResult mlsApply(ServerSim *sim, int sender, ClientCommand *cmd) {
    CmdResult r;
    cmd->cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, sender, cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult mlsTeamSide(ServerSim *sim, int sender, BYTE teamId, BYTE side) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_META;
    cmd.u.lobbyTeamMeta.teamId    = teamId;
    cmd.u.lobbyTeamMeta.startSide = side;
    return mlsApply(sim, sender, &cmd);
}

static CmdResult mlsClaim(ServerSim *sim, int sender, BYTE target, BYTE idx1) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_CLAIM_START;
    cmd.u.lobbyClaimStart.targetSlot = target;
    cmd.u.lobbyClaimStart.startIdx   = idx1;
    return mlsApply(sim, sender, &cmd);
}

/* The first start (1-based) that lies on the one side `bit` and no other,
 * 0 when the map has none. */
static BYTE mlsStartOnSide(ServerSim *sim, BYTE bit) {
    BYTE n = startsGetNumStarts(&sim->sim.ss);
    BYTE k;
    for (k = 1; k <= n; k++) {
        if (serverSimLobbyStartSideMask(sim, k) == bit) return k;
    }
    return 0;
}

/* The start (0-based) nearest where slot's tank stands; -1 with no tank.
 * startsGetStart nudges a tank off an occupied square, so the square is
 * not the answer; the nearest start is. */
static int mlsTankStart(GameSim *gs, BYTE slot) {
    WORLD wx;
    WORLD wy;
    int   mx;
    int   my;
    int   best = -1;
    int   bestDist = 0;
    BYTE  n = startsGetNumStarts(&gs->ss);
    BYTE  i;
    if (gs->tanks[slot] == NULL) return -1;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    mx = (int)(wx >> M_W_SHIFT_SIZE);
    my = (int)(wy >> M_W_SHIFT_SIZE);
    for (i = 0; i < n; i++) {
        int dx = (int)gs->ss->item[i].x - mx;
        int dy = (int)gs->ss->item[i].y - my;
        int d  = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (best < 0 || d < bestDist) {
            best = (int)i;
            bestDist = d;
        }
    }
    return best;
}

int run_mod_lobby_starts_mac_bolo_keeps_opposite_sides(void) {
    BYTE                 emap[6000] = E_MAP;
    ServerInstanceConfig cfg;
    ServerSim           *sim;
    ScenarioHost        *h;
    GameSim             *gs;
    const char          *base;
    char                 dir[512];
    char                 err[512];
    BYTE                 west;
    BYTE                 east;
    BYTE                 named;
    int                  round;

    base = SDL_GetBasePath();
    UT_ASSERT_MSG(base != NULL, "SDL cannot say where the executable is");
    snprintf(dir, sizeof(dir), "%sdata/mods", base);

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    gs = &sim->sim;
    /* What servermain hands a lobby server. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.botAiType = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);

    h = scenarioHostAttachMod(sim, dir, MLS_MOD_FILE, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "%s under \"%s\" was refused: %s",
                  MLS_MOD_FILE, dir, err);

    /* Slot 0 is the host on team 1, slot 1 joins on team 2. */
    serverSimAddPlayer(sim, 0, "A", false);
    serverSimAddPlayer(sim, 1, "B", false);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 1);
    UT_ASSERT(sim->lobbyPlayers[1].teamNumber == 2);

    /* The host puts team 1 on west; team 2 follows to east. Then each
     * player picks a start on their own side by hand. */
    UT_ASSERT(mlsTeamSide(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT_MSG(sim->teams[1].startSide == START_SIDE_W &&
                  sim->teams[2].startSide == START_SIDE_E,
                  "setup: sides are %u/%u, wanted west/east",
                  sim->teams[1].startSide, sim->teams[2].startSide);
    west = mlsStartOnSide(sim, START_SIDE_BIT_W);
    east = mlsStartOnSide(sim, START_SIDE_BIT_E);
    UT_ASSERT_MSG(west != 0 && east != 0,
                  "setup: the map needs a start on the west and one on the east");
    UT_ASSERT(mlsClaim(sim, 0, 0, west) == CMD_OK);
    UT_ASSERT(mlsClaim(sim, 1, 1, east) == CMD_OK);
    UT_ASSERT(sim->lobbyPlayers[0].startIdx == west);
    UT_ASSERT(sim->lobbyPlayers[1].startIdx == east);

    /* The mod picks at random, so one round could land on the picks by
     * chance. Several rounds in a row cannot. */
    for (round = 0; round < MLS_ROUNDS; round++) {
        int a;
        int b;
        serverSimStartGame(sim);
        UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
        a = mlsTankStart(gs, 0);
        b = mlsTankStart(gs, 1);
        UT_ASSERT_MSG(a == (int)west - 1 && b == (int)east - 1,
                      "round %d with Mac Bolo Rules on: the tanks opened on "
                      "starts %d and %d, wanted the lobby picks %u (west) and "
                      "%u (east)", round + 1, a + 1, b + 1,
                      (unsigned)west, (unsigned)east);
        if (round + 1 < MLS_ROUNDS) {
            serverSimReturnToLobby(sim);
            UT_ASSERT(sim->lobbyPlayers[0].startIdx == west);
            UT_ASSERT(sim->lobbyPlayers[1].startIdx == east);
        }
    }

    /* The mod still owns the spawns after the opening: once the round's
     * first tick has run, it names a start for a respawning tank. */
    serverSimTick(sim);
    named = MAX_STARTS;
    UT_ASSERT_MSG(gameSimChooseStart(gs, 0, &named) && named < startsGetNumStarts(&gs->ss),
                  "after the opening, Mac Bolo Rules must still name a "
                  "respawn's start");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

/* ── The mod's settings ───────────────────────────────────────────── */

/* One host's choice: a setting id and its value (0 or 1 for a bool). */
typedef struct {
    const char *id;
    int32_t     value;
} MlsSetting;

/* The data/mods directory beside the test binary, in dir. */
static bool mlsModDir(char *dir, size_t cap) {
    const char *base = SDL_GetBasePath();
    if (base == NULL) return false;
    snprintf(dir, cap, "%sdata/mods", base);
    return true;
}

/* Chooses each of set's values for the mod. The declaration is read from
 * the scenario directory, so the caller has set it to the mods directory. */
static int mlsChoose(ServerSim *sim, const MlsSetting *set, int count) {
    int i;
    for (i = 0; i < count; i++) {
        int32_t got = -1;
        UT_ASSERT_MSG(serverSimSetScriptSetting(sim, MLS_MOD_FILE, set[i].id,
                                                set[i].value, &got) &&
                          got == set[i].value,
                      "setting %s to %d was refused or moved to %d",
                      set[i].id, (int)set[i].value, (int)got);
    }
    return 0;
}

/* One round with the mod on and set's choices made, its rules in *out. */
static int mlsRulesRound(const MlsSetting *set, int count, SimRules *out) {
    BYTE          emap[6000] = E_MAP;
    ServerSim    *sim;
    ScenarioHost *h;
    char          dir[512];
    char          err[512];

    UT_ASSERT_MSG(mlsModDir(dir, sizeof(dir)),
                  "SDL cannot say where the executable is");
    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, dir);
    scenarioHostRegisterScenarioLister(sim);
    UT_ASSERT(mlsChoose(sim, set, count) == 0);

    err[0] = '\0';
    h = scenarioHostAttachMod(sim, dir, MLS_MOD_FILE, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "%s under \"%s\" was refused: %s",
                  MLS_MOD_FILE, dir, err);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));
    *out = serverSimGetGameSim(sim)->rules;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

/* The mod's parts that set rules, in the order its settings list them. */
enum {
    MLS_PART_PUSHBACK,
    MLS_PART_SHELL_CAP,
    MLS_PART_TANK_BOXES,
    MLS_PART_PILL_AIM,
    MLS_PART_BASE_DEFENCE,
    MLS_PART_BUILDER_WALK,
    MLS_PART_COUNT
};

static const char *const kMlsPartSetting[MLS_PART_COUNT] = {
    "pushback", "shell_cap", "tank_boxes", "pill_aim", "base_defence",
    "builder_walk",
};

/* Every rule the mod sets, the part that sets it, and the value its
 * written table always gave it. */
typedef struct {
    const char *name;
    size_t      offset;
    int         part;
    int32_t     mac;
} MlsRule;

#define MLS_RULE(field, part, mac) \
    { #field, offsetof(SimRules, field), part, mac }

static const MlsRule kMlsRules[] = {
    MLS_RULE(tank_slide_mac,               MLS_PART_PUSHBACK,     1),
    MLS_RULE(tank_slide_step,              MLS_PART_PUSHBACK,     28),
    MLS_RULE(tank_slide_armour_bonus,      MLS_PART_PUSHBACK,     32),
    MLS_RULE(tank_bump_decay_shift,        MLS_PART_PUSHBACK,     2),
    MLS_RULE(pill_shell_cap,               MLS_PART_SHELL_CAP,    1),
    MLS_RULE(pill_max_shells_at_tank,      MLS_PART_SHELL_CAP,    12),
    MLS_RULE(tank_collision_mac,           MLS_PART_TANK_BOXES,   1),
    MLS_RULE(pill_aim_mac,                 MLS_PART_PILL_AIM,     1),
    MLS_RULE(pill_base_defend_shape,       MLS_PART_BASE_DEFENCE, 1),
    MLS_RULE(pill_base_defend_range,       MLS_PART_BASE_DEFENCE, 7),
    MLS_RULE(man_bless_tile_terrain_speed, MLS_PART_BUILDER_WALK, 1),
};

#define MLS_RULE_COUNT ((int)(sizeof(kMlsRules) / sizeof(kMlsRules[0])))

static int32_t mlsRuleValue(const SimRules *r, const MlsRule *rule) {
    return *(const int32_t *)(const void *)((const char *)r + rule->offset);
}

int run_mac_bolo_settings_defaults_play_old_rules(void) {
    SimRules got;
    SimRules classic;
    int      i;

    simRulesClassic(&classic);
    UT_ASSERT(mlsRulesRound(NULL, 0, &got) == 0);
    for (i = 0; i < MLS_RULE_COUNT; i++) {
        int32_t v = mlsRuleValue(&got, &kMlsRules[i]);
        UT_ASSERT_MSG(v == kMlsRules[i].mac,
                      "with nothing chosen %s is %d, the mod always set %d",
                      kMlsRules[i].name, (int)v, (int)kMlsRules[i].mac);
    }
    /* A rule the mod never set is still the classic table's. */
    UT_ASSERT(got.start_pill_range == classic.start_pill_range);
    UT_ASSERT(got.tank_wall_glide == classic.tank_wall_glide);
    return 0;
}

int run_mac_bolo_settings_each_part_off(void) {
    SimRules classic;
    int      part;

    simRulesClassic(&classic);
    for (part = 0; part < MLS_PART_COUNT; part++) {
        MlsSetting off;
        SimRules   got;
        int        i;

        off.id    = kMlsPartSetting[part];
        off.value = 0;
        UT_ASSERT(mlsRulesRound(&off, 1, &got) == 0);
        for (i = 0; i < MLS_RULE_COUNT; i++) {
            const MlsRule *rule = &kMlsRules[i];
            int32_t        v    = mlsRuleValue(&got, rule);
            int32_t        want = (rule->part == part)
                                      ? mlsRuleValue(&classic, rule)
                                      : rule->mac;
            UT_ASSERT_MSG(v == want,
                          "with %s off, %s is %d, wanted %d (%s)", off.id,
                          rule->name, (int)v, (int)want,
                          rule->part == part ? "classic" : "the mod's");
        }
    }
    return 0;
}

int run_mac_bolo_settings_numbers(void) {
    static const MlsSetting set[] = {
        { "push_step",           40 },
        { "push_armour_bonus",   10 },
        { "push_decay_shift",    3  },
        { "shell_cap_count",     5  },
        { "base_defence_radius", 9  },
    };
    SimRules got;

    UT_ASSERT(mlsRulesRound(set, (int)(sizeof(set) / sizeof(set[0])),
                            &got) == 0);
    UT_ASSERT(got.tank_slide_mac == 1);
    UT_ASSERT_MSG(got.tank_slide_step == 40, "tank_slide_step is %d",
                  (int)got.tank_slide_step);
    UT_ASSERT_MSG(got.tank_slide_armour_bonus == 10,
                  "tank_slide_armour_bonus is %d",
                  (int)got.tank_slide_armour_bonus);
    UT_ASSERT_MSG(got.tank_bump_decay_shift == 3,
                  "tank_bump_decay_shift is %d",
                  (int)got.tank_bump_decay_shift);
    UT_ASSERT(got.pill_shell_cap == 1);
    UT_ASSERT_MSG(got.pill_max_shells_at_tank == 5,
                  "pill_max_shells_at_tank is %d",
                  (int)got.pill_max_shells_at_tank);
    UT_ASSERT(got.pill_base_defend_shape == 1);
    UT_ASSERT_MSG(got.pill_base_defend_range == 9,
                  "pill_base_defend_range is %d",
                  (int)got.pill_base_defend_range);
    return 0;
}

/* A lobby of two humans, team 1 west and team 2 east, the mod on with set's
 * choices made. Each player claims a start on their own side by hand. The
 * picks are in *west and *east. */
static int mlsLobby(const MlsSetting *set, int count, ServerSim **outSim,
                    ScenarioHost **outHost, BYTE *west, BYTE *east) {
    BYTE                 emap[6000] = E_MAP;
    ServerInstanceConfig cfg;
    ServerSim           *sim;
    ScenarioHost        *h;
    char                 dir[512];
    char                 err[512];

    UT_ASSERT_MSG(mlsModDir(dir, sizeof(dir)),
                  "SDL cannot say where the executable is");
    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    memset(&cfg, 0, sizeof(cfg));
    cfg.botAiType = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);
    serverSimSetScenarioDir(sim, dir);
    scenarioHostRegisterScenarioLister(sim);
    UT_ASSERT(mlsChoose(sim, set, count) == 0);

    err[0] = '\0';
    h = scenarioHostAttachMod(sim, dir, MLS_MOD_FILE, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "%s under \"%s\" was refused: %s",
                  MLS_MOD_FILE, dir, err);

    serverSimAddPlayer(sim, 0, "A", false);
    serverSimAddPlayer(sim, 1, "B", false);
    UT_ASSERT(sim->lobbyPlayers[0].teamNumber == 1);
    UT_ASSERT(sim->lobbyPlayers[1].teamNumber == 2);
    UT_ASSERT(mlsTeamSide(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT(sim->teams[1].startSide == START_SIDE_W &&
              sim->teams[2].startSide == START_SIDE_E);
    *west = mlsStartOnSide(sim, START_SIDE_BIT_W);
    *east = mlsStartOnSide(sim, START_SIDE_BIT_E);
    UT_ASSERT_MSG(*west != 0 && *east != 0,
                  "setup: the map needs a start on the west and one on the "
                  "east");
    UT_ASSERT(mlsClaim(sim, 0, 0, *west) == CMD_OK);
    UT_ASSERT(mlsClaim(sim, 1, 1, *east) == CMD_OK);
    UT_ASSERT(sim->lobbyPlayers[0].startIdx == *west);
    UT_ASSERT(sim->lobbyPlayers[1].startIdx == *east);

    *outSim  = sim;
    *outHost = h;
    return 0;
}

int run_mac_bolo_first_start_lobby_off_uses_mac_pick(void) {
    static const MlsSetting set[] = { { "first_start_lobby", 0 } };
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    BYTE          west = 0;
    BYTE          east = 0;
    BYTE          named;
    int           round;
    int           moved = 0;

    UT_ASSERT(mlsLobby(set, 1, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    /* The classic engine puts every opening tank on its lobby pick, and so
     * does the mod with first_start_lobby on (the test above, and the one
     * with spawn_starts off below). With it off, the Mac Bolo pick places
     * the opening tanks at random over the clear starts, so across several
     * rounds some tank opens away from its pick. */
    for (round = 0; round < MLS_ROUNDS; round++) {
        BYTE slot;
        serverSimStartGame(sim);
        UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
        for (slot = 0; slot < 2; slot++) {
            int  s    = mlsTankStart(gs, slot);
            BYTE pick = slot == 0 ? west : east;
            UT_ASSERT_MSG(s >= 0, "round %d: seat %u has no tank", round + 1,
                          (unsigned)slot);
            if (s != (int)pick - 1) {
                moved++;
            }
        }
        if (round + 1 < MLS_ROUNDS) {
            serverSimReturnToLobby(sim);
        }
    }
    UT_ASSERT_MSG(moved > 0,
                  "with first_start_lobby off every opening tank stood on "
                  "its lobby pick in %d rounds: the Mac Bolo pick did not "
                  "place them", MLS_ROUNDS);

    /* And the spawns after the opening are the mod's too. */
    serverSimTick(sim);
    named = MAX_STARTS;
    UT_ASSERT(gameSimChooseStart(gs, 0, &named) &&
              named < startsGetNumStarts(&gs->ss));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

int run_mac_bolo_spawn_starts_off_engine_picks(void) {
    /* first_start_lobby off as well: it has no effect while spawn_starts
     * is off, so the opening is still the lobby's. */
    static const MlsSetting set[] = {
        { "spawn_starts",      0 },
        { "first_start_lobby", 0 },
    };
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    BYTE          west = 0;
    BYTE          east = 0;
    BYTE          named;

    UT_ASSERT(mlsLobby(set, 2, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(mlsTankStart(gs, 0) == (int)west - 1 &&
                      mlsTankStart(gs, 1) == (int)east - 1,
                  "with spawn_starts off the tanks opened on starts %d and "
                  "%d, wanted the lobby picks %u and %u",
                  mlsTankStart(gs, 0) + 1, mlsTankStart(gs, 1) + 1,
                  (unsigned)west, (unsigned)east);

    /* Once running, the mod names no start: the engine picks. */
    serverSimTick(sim);
    named = MAX_STARTS;
    UT_ASSERT_MSG(!gameSimChooseStart(gs, 0, &named),
                  "with spawn_starts off the mod named start %u for a spawn",
                  (unsigned)named);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

/* The shell cap rules in r are the classic table's, and the pushback rules
 * are the mod's at push_step. */
static int mlsShellCapOff(const SimRules *r, int32_t pushStep,
                          const char *when) {
    SimRules classic;

    simRulesClassic(&classic);
    UT_ASSERT_MSG(r->pill_shell_cap == classic.pill_shell_cap &&
                      r->pill_max_shells_at_tank ==
                          classic.pill_max_shells_at_tank,
                  "%s: pill_shell_cap %d and pill_max_shells_at_tank %d, "
                  "wanted the classic %d and %d", when,
                  (int)r->pill_shell_cap, (int)r->pill_max_shells_at_tank,
                  (int)classic.pill_shell_cap,
                  (int)classic.pill_max_shells_at_tank);
    UT_ASSERT_MSG(r->tank_slide_mac == 1 && r->tank_slide_step == pushStep,
                  "%s: tank_slide_mac %d and tank_slide_step %d, wanted 1 "
                  "and %d", when, (int)r->tank_slide_mac,
                  (int)r->tank_slide_step, (int)pushStep);
    return 0;
}

int run_mac_bolo_setting_changed_after_attach(void) {
    static const MlsSetting set[] = { { "shell_cap", 0 } };
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    BYTE          west = 0;
    BYTE          east = 0;

    /* The mod is attached with nothing chosen, as the lobby attaches it.
     * Only then does the host open the details dialog and change a value. */
    UT_ASSERT(mlsLobby(NULL, 0, &sim, &h, &west, &east) == 0);
    UT_ASSERT(mlsChoose(sim, set, 1) == 0);

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));
    UT_ASSERT(mlsShellCapOff(&serverSimGetGameSim(sim)->rules, 28,
                             "shell_cap off after the attach") == 0);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

int run_mac_bolo_setting_changed_between_rounds(void) {
    static const MlsSetting set[] = {
        { "shell_cap", 0  },
        { "push_step", 40 },
    };
    ServerSim      *sim = NULL;
    ScenarioHost   *h   = NULL;
    const SimRules *r;
    BYTE            west = 0;
    BYTE            east = 0;

    UT_ASSERT(mlsLobby(NULL, 0, &sim, &h, &west, &east) == 0);

    /* Round 1 plays the defaults. */
    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    r = &serverSimGetGameSim(sim)->rules;
    UT_ASSERT_MSG(r->pill_shell_cap == 1 && r->pill_max_shells_at_tank == 12,
                  "round 1 on the defaults: pill_shell_cap %d and "
                  "pill_max_shells_at_tank %d, wanted 1 and 12",
                  (int)r->pill_shell_cap, (int)r->pill_max_shells_at_tank);
    UT_ASSERT_MSG(r->tank_slide_step == 28,
                  "round 1 on the defaults: tank_slide_step %d, wanted 28",
                  (int)r->tank_slide_step);

    /* Back in the lobby the host changes two values; round 2 plays them. */
    serverSimReturnToLobby(sim);
    UT_ASSERT(mlsChoose(sim, set, (int)(sizeof(set) / sizeof(set[0]))) == 0);
    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "round 2 complained: %s", scenarioHostLastError(h));
    UT_ASSERT(mlsShellCapOff(&serverSimGetGameSim(sim)->rules, 40,
                             "round 2 after the lobby change") == 0);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    return 0;
}

/* ── on_choose_start's lobby argument ─────────────────────────────── */

/* A small mod written for the case, and what it says it was told. The mod
 * prints a marked line for each question; a scenario state's print goes to
 * the server console, which the watcher below catches. */
#define MLS_NOTE_MARK "note:"

static char   mlsNote[8192];
static size_t mlsNoteLen;
static void (*mlsConsolePrev)(void *ctx, char *msg) = NULL;
static char   mlsEchoDir[256];

static void mlsNoteClear(void) {
    mlsNote[0] = '\0';
    mlsNoteLen = 0;
}

/* One console line, kept if the mod wrote it. print turns the newline a
   note ends with into a space, so trailing spaces are dropped and the line
   is ended with a newline here instead. */
static void mlsConsoleCb(void *ctx, char *msg) {
    size_t mark = strlen(MLS_NOTE_MARK);
    size_t n;
    if (mlsConsolePrev != NULL) {
        mlsConsolePrev(ctx, msg);
    }
    if (msg == NULL || strncmp(msg, MLS_NOTE_MARK, mark) != 0) {
        return;
    }
    n = strlen(msg + mark);
    while (n > 0 && msg[mark + n - 1] == ' ') {
        n--;
    }
    if (mlsNoteLen + n + 2 > sizeof(mlsNote)) {
        return;
    }
    memcpy(mlsNote + mlsNoteLen, msg + mark, n);
    mlsNoteLen += n;
    mlsNote[mlsNoteLen++] = '\n';
    mlsNote[mlsNoteLen]   = '\0';
}

/* The mod's file, with note() in front of body. */
static bool mlsWriteEcho(const char *tag, const char *body) {
    char  path[512];
    FILE *f;

    snprintf(mlsEchoDir, sizeof(mlsEchoDir), "wbtest_mls_%s", tag);
    /* A directory left by a run that was killed is not a failure: the file
       below is written over whatever is in it. */
    (void)SDL_RemovePath(mlsEchoDir);
    if (!SDL_CreateDirectory(mlsEchoDir)) return false;
    snprintf(path, sizeof(path), "%s/echo.lua", mlsEchoDir);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fprintf(f,
            "scenario = { name = \"Lobby Echo\", api = 1, kind = \"mod\",\n"
            "             bound = false }\n"
            "local function note(s)\n"
            "  print(\"" MLS_NOTE_MARK "\" .. tostring(s))\n"
            "end\n"
            "%s", body);
    fclose(f);
    mlsNoteClear();
    return true;
}

static void mlsDropEcho(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/echo.lua", mlsEchoDir);
    remove(path);
    SDL_RemovePath(mlsEchoDir);
}

/* The notes most cases below share: what on_choose_start is handed, what
   game.lobby_start and game.lobby_side read beside it, and what
   game.lobby_start reads once the opening tanks are built (seat 5 is
   empty). */
#define MLS_ECHO_NOTES                                                       \
    "local function said(p, lobby)\n"                                        \
    "  note(\"choose \" .. p .. \" \" .. tostring(lobby) .. \" \" ..\n"      \
    "       tostring(game.lobby_start(p)) .. \" \" ..\n"                     \
    "       tostring(game.lobby_side(p)))\n"                                 \
    "end\n"                                                                  \
    "function on_start()\n"                                                  \
    "  note(\"started \" .. tostring(game.lobby_start(0)) .. \" \" ..\n"     \
    "       tostring(game.lobby_start(1)) .. \" \" ..\n"                     \
    "       tostring(game.lobby_start(5)) .. \" \" ..\n"                     \
    "       tostring(game.lobby_side(5)))\n"                                 \
    "end\n"

/* Two humans, team 1 west and team 2 east, the echo mod attached. With
 * handPick each claims a start on their own side, in *west and *east;
 * without, the lobby's own pick on each side stands and both are 0. */
static int mlsEchoLobby(bool handPick, ServerSim **outSim,
                        ScenarioHost **outHost, BYTE *west, BYTE *east) {
    BYTE                 emap[6000] = E_MAP;
    ServerInstanceConfig cfg;
    ServerSim           *sim;
    ScenarioHost        *h;
    char                 err[512];

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetActive(sim);
    memset(&cfg, 0, sizeof(cfg));
    cfg.botAiType = (BYTE)aiNone;
    serverSimApplyInstanceConfig(sim, &cfg);
    mlsConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = mlsConsoleCb;

    err[0] = '\0';
    h = scenarioHostAttachMod(sim, mlsEchoDir, "echo.lua", err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the echo mod was refused: %s", err);

    serverSimAddPlayer(sim, 0, "A", false);
    serverSimAddPlayer(sim, 1, "B", false);
    UT_ASSERT(mlsTeamSide(sim, 0, 1, START_SIDE_W) == CMD_OK);
    UT_ASSERT(sim->teams[1].startSide == START_SIDE_W &&
              sim->teams[2].startSide == START_SIDE_E);
    *west = 0;
    *east = 0;
    if (handPick) {
        *west = mlsStartOnSide(sim, START_SIDE_BIT_W);
        *east = mlsStartOnSide(sim, START_SIDE_BIT_E);
        UT_ASSERT(*west != 0 && *east != 0);
        UT_ASSERT(mlsClaim(sim, 0, 0, *west) == CMD_OK);
        UT_ASSERT(mlsClaim(sim, 1, 1, *east) == CMD_OK);
    }
    *outSim  = sim;
    *outHost = h;
    return 0;
}

static void mlsEchoEnd(ServerSim *sim, ScenarioHost *h) {
    sim->sim.callbacks.consoleMessage = mlsConsolePrev;
    mlsConsolePrev = NULL;
    scenarioHostDetach(h);
    serverSimDestroy(sim);
    mlsDropEcho();
}

/* The number on_choose_start was handed in its first question about seat
 * p, 0 for nil, or -1 when the mod was never asked about p. */
static int mlsToldLobby(BYTE p) {
    char        want[32];
    const char *at;
    int         n = 0;

    snprintf(want, sizeof(want), "choose %u ", (unsigned)p);
    at = strstr(mlsNote, want);
    if (at == NULL) return -1;
    at += strlen(want);
    if (strncmp(at, "nil", 3) == 0) return 0;
    if (sscanf(at, "%d", &n) != 1) return -1;
    return n;
}

/* The map square slot's tank stands on, as one number; -1 with no tank. */
static int mlsTankSquare(GameSim *gs, BYTE slot) {
    WORLD wx;
    WORLD wy;
    if (gs->tanks[slot] == NULL) return -1;
    tankGetWorld(&gs->tanks[slot], &wx, &wy);
    return (int)(wy >> M_W_SHIFT_SIZE) * 256 + (int)(wx >> M_W_SHIFT_SIZE);
}

/* The policy every echo case but the override ones answers with: say what
   it was told and leave the pick to the engine. */
#define MLS_ECHO_NIL                                                         \
    MLS_ECHO_NOTES                                                           \
    "function on_choose_start(p, lobby)\n"                                   \
    "  said(p, lobby)\n"                                                     \
    "  return nil\n"                                                         \
    "end\n"

int run_choose_start_lobby_arg_hand_picked(void) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    char          want[96];
    BYTE          west;
    BYTE          east;
    BYTE          named;

    UT_ASSERT(mlsWriteEcho("hand", MLS_ECHO_NIL));
    UT_ASSERT(mlsEchoLobby(true, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    /* Each seat is told its hand pick, game.lobby_start reads the same
       number beside it, and game.lobby_side names the team's side. */
    snprintf(want, sizeof(want), "choose 0 %u %u west\n", (unsigned)west,
             (unsigned)west);
    UT_ASSERT_MSG(strstr(mlsNote, want) != NULL,
                  "wanted \"%s\" in what the mod was told:\n%s", want,
                  mlsNote);
    snprintf(want, sizeof(want), "choose 1 %u %u east\n", (unsigned)east,
             (unsigned)east);
    UT_ASSERT_MSG(strstr(mlsNote, want) != NULL,
                  "wanted \"%s\" in what the mod was told:\n%s", want,
                  mlsNote);

    /* Answering nil leaves the engine its own pick, which is the lobby's. */
    UT_ASSERT(mlsTankStart(gs, 0) == (int)west - 1);
    UT_ASSERT(mlsTankStart(gs, 1) == (int)east - 1);

    /* The opening tanks spent the reservations: on_start reads nil for both
       seats, and nil for an empty seat's start and side. on_start runs on
       the round's first tick. */
    serverSimTick(sim);
    UT_ASSERT_MSG(strstr(mlsNote, "started nil nil nil nil\n") != NULL,
                  "on_start should read no lobby start for any seat:\n%s",
                  mlsNote);

    /* A later spawn is told nothing, and the side is still the team's. */
    mlsNoteClear();
    named = MAX_STARTS;
    UT_ASSERT(!gameSimChooseStart(gs, 0, &named));
    UT_ASSERT_MSG(strstr(mlsNote, "choose 0 nil nil west\n") != NULL,
                  "a respawn should be told no lobby start:\n%s", mlsNote);

    mlsEchoEnd(sim, h);
    return 0;
}

int run_choose_start_lobby_arg_team_side(void) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    BYTE          west;
    BYTE          east;
    int           a;
    int           b;

    UT_ASSERT(mlsWriteEcho("side", MLS_ECHO_NIL));
    UT_ASSERT(mlsEchoLobby(false, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    /* No hand pick: each seat is told a start its team's side holds, and
       its tank stands there. */
    a = mlsToldLobby(0);
    b = mlsToldLobby(1);
    UT_ASSERT_MSG(a > 0 && b > 0, "both seats should be told a start, "
                  "were told %d and %d:\n%s", a, b, mlsNote);
    UT_ASSERT_MSG((serverSimLobbyStartSideMask(sim, (BYTE)a) &
                   START_SIDE_BIT_W) != 0,
                  "seat 0 (west) was told start %d, which is not on the "
                  "west", a);
    UT_ASSERT_MSG((serverSimLobbyStartSideMask(sim, (BYTE)b) &
                   START_SIDE_BIT_E) != 0,
                  "seat 1 (east) was told start %d, which is not on the "
                  "east", b);
    /* The lobby shows each seat the start it would take, and that is the
       one the seat is told. */
    UT_ASSERT_MSG(sim->lobbyPlayers[0].startIdx == (BYTE)a &&
                      sim->lobbyPlayers[1].startIdx == (BYTE)b,
                  "the lobby showed starts %u and %u, the seats were told "
                  "%d and %d", (unsigned)sim->lobbyPlayers[0].startIdx,
                  (unsigned)sim->lobbyPlayers[1].startIdx, a, b);
    UT_ASSERT(mlsTankStart(gs, 0) == a - 1);
    UT_ASSERT(mlsTankStart(gs, 1) == b - 1);
    UT_ASSERT(strstr(mlsNote, " west\n") != NULL &&
              strstr(mlsNote, " east\n") != NULL);

    mlsEchoEnd(sim, h);
    return 0;
}

int run_choose_start_lobby_arg_nil_without_reservation(void) {
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    BYTE          west;
    BYTE          east;

    UT_ASSERT(mlsWriteEcho("none", MLS_ECHO_NIL));
    UT_ASSERT(mlsEchoLobby(true, &sim, &h, &west, &east) == 0);

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    /* A seat that joins once the round is running was not in the opening
       placement, so it has no reservation. */
    mlsNoteClear();
    serverSimAddPlayer(sim, 2, "C", false);
    UT_ASSERT_MSG(sim->sim.tanks[2] != NULL, "setup: seat 2 has no tank");
    UT_ASSERT_MSG(mlsToldLobby(2) == 0,
                  "a seat joining a running round was told start %d:\n%s",
                  mlsToldLobby(2), mlsNote);

    mlsEchoEnd(sim, h);
    return 0;
}

int run_choose_start_override_releases_reservation(void) {
    /* Seat 0 names a start that is no seat's reservation; seat 1 keeps the
       one the lobby gave it. */
    static const char kBody[] =
        MLS_ECHO_NOTES
        "function on_choose_start(p, lobby)\n"
        "  said(p, lobby)\n"
        "  if p ~= 0 or lobby == nil then return nil end\n"
        "  for n = 1, game.num_starts() do\n"
        "    local taken = false\n"
        "    for q = 0, game.max_tanks() - 1 do\n"
        "      if game.lobby_start(q) == n then taken = true end\n"
        "    end\n"
        "    if not taken and game.start(n) then\n"
        "      note(\"override \" .. n)\n"
        "      return n\n"
        "    end\n"
        "  end\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    const char   *at;
    BYTE          west;
    BYTE          east;
    BYTE          named;
    int           other = 0;

    UT_ASSERT(mlsWriteEcho("override", kBody));
    UT_ASSERT(mlsEchoLobby(true, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    at = strstr(mlsNote, "override ");
    UT_ASSERT_MSG(at != NULL && sscanf(at + 9, "%d", &other) == 1 &&
                      other > 0 && other != (int)west && other != (int)east,
                  "setup: the mod should have named a third start:\n%s",
                  mlsNote);

    /* The script's start wins over the lobby's, the seat's own reservation
       is spent rather than left for a later spawn, and the other seat still
       opens on its own pick. */
    UT_ASSERT_MSG(mlsTankStart(gs, 0) == other - 1,
                  "seat 0 opened nearest start %d, the mod named %d",
                  mlsTankStart(gs, 0) + 1, other);
    UT_ASSERT(gs->pendingStartIdx[0] == MAX_STARTS);
    UT_ASSERT(mlsTankStart(gs, 1) == (int)east - 1);
    UT_ASSERT(gs->pendingStartIdx[1] == MAX_STARTS);

    /* Its respawn is told nothing: no reservation was left behind. */
    serverSimTick(sim);
    mlsNoteClear();
    named = MAX_STARTS;
    UT_ASSERT(!gameSimChooseStart(gs, 0, &named));
    UT_ASSERT_MSG(mlsToldLobby(0) == 0,
                  "after an override the respawn was told start %d:\n%s",
                  mlsToldLobby(0), mlsNote);

    mlsEchoEnd(sim, h);
    return 0;
}

int run_choose_start_override_onto_other_reservation(void) {
    /* Seat 0 asks for seat 1's reserved start. Both tanks open by it, on
       squares of their own, and neither reservation is left over. */
    static const char kBody[] =
        MLS_ECHO_NOTES
        "function on_choose_start(p, lobby)\n"
        "  said(p, lobby)\n"
        "  if p == 0 and lobby ~= nil then return game.lobby_start(1) end\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    BYTE          west;
    BYTE          east;

    UT_ASSERT(mlsWriteEcho("shared", kBody));
    UT_ASSERT(mlsEchoLobby(true, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);

    UT_ASSERT_MSG(mlsTankStart(gs, 0) == (int)east - 1 &&
                      mlsTankStart(gs, 1) == (int)east - 1,
                  "both tanks should open by start %u, opened by %d and %d",
                  (unsigned)east, mlsTankStart(gs, 0) + 1,
                  mlsTankStart(gs, 1) + 1);
    UT_ASSERT_MSG(mlsTankSquare(gs, 0) != mlsTankSquare(gs, 1),
                  "the two tanks were put on one square");
    UT_ASSERT(gs->pendingStartIdx[0] == MAX_STARTS &&
              gs->pendingStartIdx[1] == MAX_STARTS);

    mlsEchoEnd(sim, h);
    return 0;
}

int run_choose_start_script_ignoring_lobby_unchanged(void) {
    /* A one-parameter policy, as every script before the argument was
       written: its answer stands whatever the lobby chose, and a nil
       answer leaves the lobby's start. */
    static const char kBody[] =
        "function on_choose_start(p)\n"
        "  note(\"asked \" .. p)\n"
        "  if p == 0 then return game.num_starts() end\n"
        "  return nil\n"
        "end\n";
    ServerSim    *sim = NULL;
    ScenarioHost *h   = NULL;
    GameSim      *gs;
    BYTE          west;
    BYTE          east;
    BYTE          last;

    UT_ASSERT(mlsWriteEcho("ignore", kBody));
    UT_ASSERT(mlsEchoLobby(true, &sim, &h, &west, &east) == 0);
    gs = &sim->sim;
    last = startsGetNumStarts(&gs->ss);
    UT_ASSERT_MSG(last != west && last != east,
                  "setup: the last start must be neither pick");

    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT(strstr(mlsNote, "asked 0\n") != NULL &&
              strstr(mlsNote, "asked 1\n") != NULL);
    UT_ASSERT_MSG(mlsTankStart(gs, 0) == (int)last - 1,
                  "seat 0 opened nearest start %d, the script named %u",
                  mlsTankStart(gs, 0) + 1, (unsigned)last);
    UT_ASSERT(mlsTankStart(gs, 1) == (int)east - 1);
    UT_ASSERT_MSG(scenarioHostLastError(h)[0] == '\0',
                  "the round complained: %s", scenarioHostLastError(h));

    mlsEchoEnd(sim, h);
    return 0;
}

/*
 * A mod's start pick and the lobby's (test_mod_lobby_starts.c).
 *
 * Players reported that picking starts on opposite sides in the lobby did
 * nothing: both spawned on the same side, as if the starts were random. The
 * Mac Bolo Rules mod answers on_choose_start with a random clear start, and
 * the engine asks that policy ahead of the lobby's batch slot, so with the
 * mod on, the round's opening tanks ignored every lobby pick and every team
 * side. The opening placement is the lobby's; the mod's pick is for the
 * spawns after it.
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
    /* first_start_lobby off as well: it is not read while spawn_starts is
     * off, so the opening is still the lobby's. */
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

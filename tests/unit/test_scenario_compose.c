/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A round composed of more than one script: the committed map's own scenario
 * with the mods the host picked behind it.
 *
 * What the composed list is, and what it refuses:
 *
 *   scenario_compose_map_and_mods   — a map's own scenario and two mods play
 *       together. The list is three long, the composite carries the regions
 *       and the tags of all three, and the declarative half is the map's
 *       scenario's: its name, its game type and its seats.
 *   scenario_compose_keeps_map_scenario
 *       — picking a mod does not take the map's own scenario off, and the
 *       published list says so: the map's row first with bound set, the
 *       host's pick behind it. Picking a scenario does take it off, which is
 *       the half of the older rule that stays.
 *   scenario_compose_mod_rules_load
 *       — a mod that declares a rules block plays. It used to be refused,
 *       because two scripts setting one rule came down to which was read
 *       last; the list order is now what decides, so the answer is stated
 *       rather than avoided.
 *   scenario_compose_region_clash_loads
 *       — two scripts naming one region both play, each keeping its own
 *       rectangle under the name.
 *   scenario_compose_map_script_placed
 *       — the host's list may name the committed map's own script, and the
 *       round composes it where the list puts it rather than at the front.
 *       Every region keeps the bit it had when the map's script was first,
 *       which is the whole reason the map's script may be moved at all.
 *   scenario_compose_rules_first_wins
 *       — two scripts setting one rule: the one further up the list is the
 *       value the round runs, and flipping the list flips the answer.
 *   scenario_compose_rules_keeps_earlier
 *       — a rule only the earlier script set survives the later script's
 *       rules block, because the merge is key by key.
 *   scenario_compose_region_own_first
 *       — two scripts naming one region each read their own rectangle back.
 *   scenario_compose_region_borrowed
 *       — a script that names no region of that name reads the first one on
 *       the list.
 *   scenario_compose_region_hook_own
 *       — the enter hook for one script's region does not reach a script
 *       that named a region of the same name itself.
 *   scenario_compose_conflicts_recorded
 *       — a rule overwritten and a region name used twice are both written
 *       down, with the file that won and the file that lost.
 *   scenario_compose_reload_list   — a reload re-reads every script on the
 *       list, and a file with an error in it leaves the whole list as it was.
 *
 * And what a composed round does at the calls the host makes into it:
 *
 *   scenario_compose_mod_guard     — a mod behind a scenario is refused
 *       game.score, game.end_round and game.set_game_time, each refusal
 *       naming the row it turned down, while the base scenario calls all
 *       three in the same round.
 *   scenario_compose_policy_mod_asked
 *       — a mod whose whole behaviour is can_die is asked behind a
 *       scenario, and the builder it protects survives the blow that kills
 *       him with the mod off the list.
 *   scenario_compose_policy_any_false
 *       — two scripts answering one predicate: the false wins whichever of
 *       them holds it, and the asking stops at the first no.
 *   scenario_compose_policy_damage_scale
 *       — two scripts halving a blow leave a quarter of it.
 *   scenario_compose_policy_base_win
 *       — allow_base_win is the base's alone: a mod's is not asked, and the
 *       base's own false still takes the sweep out of the round. A mod
 *       playing alone is told once that its allow_base_win is not read, and
 *       only when it wrote one.
 *   scenario_compose_policy_first_answer
 *       — two scripts handing a spawning tank different stores: the one
 *       earlier on the list wins, and flipping the list flips the answer.
 *
 * And what each script on the list is given of the shared tables:
 *
 *   scenario_compose_library_copy_per_script
 *       — a mod clearing string.find, math.floor and table.insert clears its
 *       own copies; the base still calls all three.
 *   scenario_compose_game_copy_per_script
 *       — a mod clearing game.end_round and replacing game.spawn_bot sees
 *       both changes, and the base keeps the host's rows.
 *   scenario_compose_game_nested_copy
 *       — a mod writing game.TERRAIN.road leaves the base's TERRAIN alone.
 *   scenario_compose_compat_write_stays_local
 *       — a row a mod adds to game is callable from the mod and absent from
 *       the base.
 *   scenario_compose_pairs_game_complete
 *       — pairs(game) in each script counts every field the host installed.
 *   scenario_compose_unsafe_keeps_sharing
 *       — under -allow-unsafe-scripts the scripts share one game, so the
 *       base calls the row the mod added.
 *
 * And what the lobby's Mods/Scenario setting takes out of the list:
 *
 *   scenario_compose_off_map_script_only
 *       — box off, a picked scenario and a mod on a map with its own script:
 *       the map's own script plays alone, and the list is kept.
 *   scenario_compose_off_plain_map_none
 *       — box off on a plain map: nothing plays.
 *   scenario_compose_on_picks_replace_map
 *       — box on: the picks play and the picked scenario replaces the map's
 *       own.
 *   scenario_compose_off_then_on
 *       — the list a chooser sends from the off state, without the map's
 *       row, is taken; the one with it is two scenarios and refused; and the
 *       box goes back on without a refusal.
 *   scenario_compose_off_full_list_hides_last
 *       — box off, a map with its own script and ten picks: the lobby is
 *       told the map's row and nine picks, and a list sent from what it was
 *       told loses the tenth. Nine picks look the same to the lobby, which
 *       is why a chooser refuses every edit in that state. Box on shows all
 *       ten.
 *
 * These cases drive the real path end to end: a scratch scenarios directory
 * with real files in it, the host's own directory lister registered on the
 * sim, and the picks made by applying CMD_SET_SCRIPT_LIST. A map is named but
 * never written — a loose X.scenario.lua beside it is the first thing the
 * attach looks for, so the .map file itself is not needed.
 *
 * A script says what happened to it by printing a marked line. A scenario
 * state has no io, and the sandbox routes print to the server console, so
 * the record is read off the sim's console callback. The mark is what tells
 * a script's line from the host's own: both arrive on the same console.
 *
 * Reads the ServerSim, tank and lgm structs directly; the unittests profile
 * permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_SET_SCRIPT_LIST */
#include "wire_limits.h"           /* LST_MODS_OFF */
#include "control_event.h"         /* LobbyScenarioSource */
#include "server_sim.h"
#include "server_sim_internal.h"   /* scenarioIdentity, lobbyPlayers, tick */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* the lister, the list and the map row */
#include "game_sim.h"
#include "bases.h"
#include "bolo_map.h"
#include "lgm.h"                   /* lgmDeathCheck: the builder death site */
#include "mines.h"
#include "pillbox.h"
#include "tank.h"                  /* tankDamageAmount: what a blow is worth */
#include "sim_rules_names.h"       /* SIM_RULE_* — the rules a script
                                    * sets, and what they are called */
#include "scenario_defs.h"         /* ScnDirEntry */
#include "scenario_host.h"
#include "scenario_lua.h"          /* the rows, constants and word tables
                                    * scenarioLuaInstall puts on game */
#include "scenario_manifest.h"     /* ScenarioManifest */
#include "scenario_validate.h"     /* scenarioHostManifest and
                                    * scenarioHostScriptCount: the
                                    * composite and how many scripts
                                    * went into it */
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ── The scenarios directory ──────────────────────────────────────── */

#define SC_MAX_FILES 12

static char scDir[256];
static char scFiles[SC_MAX_FILES][128];
static int  scFileCount;

/* Under this test's own scratch directory, not the working directory: these
   cases are separate ctest tests and run at the same time as each other, so
   a path shared by two of them is a file one deletes while the other is
   still reading it. */
static bool scMakeDir(const char *tag) {
    char leaf[128];

    snprintf(leaf, sizeof(leaf), "wbtest_scn_comp_%s", tag);
    if (!utScratchPath(scDir, sizeof(scDir), leaf)) return false;
    scFileCount = 0;
    /* A directory left over from a run that was killed is not a failure: the
       files below are written over whatever is in it. */
    (void)SDL_RemovePath(scDir);
    return SDL_CreateDirectory(scDir);
}

/* One scenario in the directory. Remembered so the cleanup can remove it, and
   written over in place when a case means to change it, which is what the
   reload case does. */
static bool scWrite(const char *name, const char *text) {
    char  path[512];
    FILE *f;
    int   i;
    bool  seen = false;

    for (i = 0; i < scFileCount; i++) {
        if (strcmp(scFiles[i], name) == 0) seen = true;
    }
    if (!seen) {
        if (scFileCount >= SC_MAX_FILES) return false;
        snprintf(scFiles[scFileCount], sizeof(scFiles[0]), "%s", name);
        scFileCount++;
    }
    snprintf(path, sizeof(path), "%s/%s", scDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void scDropDir(void) {
    char path[512];
    int  i;

    for (i = 0; i < scFileCount; i++) {
        snprintf(path, sizeof(path), "%s/%s", scDir, scFiles[i]);
        remove(path);
    }
    scFileCount = 0;
    SDL_RemovePath(scDir);
}

/* ── The map's own script ─────────────────────────────────────────── */

#define SC_MAP_LEAF "wbtest_scn_comp.map"

/* The map the cases pretend to have loaded, and the script beside it. One
   leaf name, but a path of this test's own for the reason scMakeDir gives:
   every case in this file writes and removes this script, and they run
   together. */
static const char *scMapPath(void) {
    static char path[512];

    if (!utScratchPath(path, sizeof(path), SC_MAP_LEAF)) {
        path[0] = '\0';
    }
    return path;
}

static void scScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

static bool scPutMapScript(const char *text) {
    char  path[512];
    FILE *f;

    scScriptFor(scMapPath(), path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void scDropMapScript(void) {
    char path[512];
    scScriptFor(scMapPath(), path, sizeof(path));
    remove(path);
}

/* ── The scripts these cases compose ──────────────────────────────── */

/* The map's own scenario: two held seats on team 2, a region and a pill tag.
 * bound, because it belongs to the map it arrived with. */
static const char kScMapScenario[] =
    "scenario = {\n"
    "  name = \"Survival\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  lobby = {\n"
    "    teams = { { id = 2, bots = 2, max_bots = 2, fielded = false } },\n"
    "  },\n"
    "  tags = { pills = { [1] = \"home\" } },\n"
    "  regions = { north = { x = 10, y = 10, w = 20, h = 20 } },\n"
    "}\n";

/* A mod: it says so, so it may not end the round. A region and a tag of its
 * own, which is what the composite has to end up holding as well as the
 * map's. */
static const char kScModOne[] =
    "scenario = {\n"
    "  name = \"Quick Reload\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  tags = { pills = { [2] = \"outer\" } },\n"
    "  regions = { south = { x = 30, y = 30, w = 10, h = 10 } },\n"
    "}\n";

/* A second mod, tagging the same pill as the map's scenario with a tag of its
 * own. Two scripts tagging one pill is two tags and not a clash. */
static const char kScModTwo[] =
    "scenario = {\n"
    "  name = \"No Mines\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  tags = { pills = { [1] = \"watched\" } },\n"
    "  regions = { east = { x = 50, y = 50, w = 8, h = 8 } },\n"
    "}\n";

/* The same mod after a host edited it: a different name, so a reload that
 * took is visible. */
static const char kScModTwoEdited[] =
    "scenario = {\n"
    "  name = \"No Mines At All\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  tags = { pills = { [1] = \"watched\" } },\n"
    "  regions = { east = { x = 50, y = 50, w = 8, h = 8 } },\n"
    "}\n";

/* And the same mod with a syntax error in it, which is what a reload has to
 * refuse without disturbing the list that is playing. */
static const char kScModTwoBroken[] =
    "scenario = {\n"
    "  name = \"No Mines\",\n"
    "  api = 1,\n";

/* A mod that declares a rules block, which is a thing a mod may now do. Two
 * scripts setting one rule is settled by the list order rather than refused,
 * so this file plays behind the map's own scenario. */
static const char kScModRules[] =
    "scenario = {\n"
    "  name = \"Thick Armour\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  rules = { tank_death_ticks = 400 },\n"
    "}\n";

/* A mod naming the region the map's scenario has already named, under a
 * rectangle of its own. Both go into the composite and each file goes on
 * meaning its own. */
static const char kScModClash[] =
    "scenario = {\n"
    "  name = \"North Again\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  regions = { north = { x = 60, y = 60, w = 4, h = 4 } },\n"
    "}\n";

/* A second scenario, which is not a mod and says nothing about kind. Picked
 * over a map that brought its own, it replaces it. */
static const char kScOtherScenario[] =
    "scenario = {\n"
    "  name = \"Duel\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  game = \"tournament\",\n"
    "}\n";

/* A mod with nothing in it but its name, so any number of copies under
 * different file names compose side by side without a clash. */
static const char kScModPlain[] =
    "scenario = {\n"
    "  name = \"Plain\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* A mod that fields its own bots and says so. */
static const char kScModNeedsBots[] =
    "scenario = {\n"
    "  name = \"Horde\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  needs_bots = true,\n"
    "}\n";

/* A map's own scenario that fields its own bots and says so. */
static const char kScScenarioNeedsBots[] =
    "scenario = {\n"
    "  name = \"Waves\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  needs_bots = true,\n"
    "}\n";

/* ── What a script says ───────────────────────────────────────────── */

/* The mark in front of every line a script of these cases prints, so the
   record holds what the scripts said and not what the host said on the same
   console. */
#define SC_NOTE_MARK "note:"

static char   scNote[8192];
static size_t scNoteLen;

/* The host's own line about a mod's allow_base_win, which carries no mark:
   counted rather than kept, because what a case reads is how many times it
   was said. */
#define SC_BASE_WIN_SKIPPED "allow_base_win is not read"

static int scBaseWinSkipped;

static void scNoteReset(void) {
    scNote[0] = '\0';
    scNoteLen = 0;
    scBaseWinSkipped = 0;
}

/* One console line, kept if a script wrote it. A record past the buffer is
   truncated rather than overrunning it. */
static void scNoteLine(const char *msg) {
    size_t mark = strlen(SC_NOTE_MARK);
    size_t room;
    size_t n;

    if (msg == NULL || strncmp(msg, SC_NOTE_MARK, mark) != 0) {
        return;
    }
    n    = strlen(msg + mark);
    room = sizeof(scNote) - 1 - scNoteLen;
    if (n > room) {
        n = room;
    }
    memcpy(scNote + scNoteLen, msg + mark, n);
    scNoteLen += n;
    scNote[scNoteLen] = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void (*scConsolePrev)(void *ctx, char *msg) = NULL;

static void scConsoleCb(void *ctx, char *msg) {
    if (scConsolePrev != NULL) {
        scConsolePrev(ctx, msg);
    }
    if (msg != NULL && strstr(msg, SC_BASE_WIN_SKIPPED) != NULL) {
        scBaseWinSkipped++;
    }
    scNoteLine(msg);
}

/* The record so far, or "" when no script has written one. */
static void scReadNote(char *out, size_t outLen) {
    snprintf(out, outLen, "%s", scNote);
}

/* ── The sim ──────────────────────────────────────────────────────── */

/* The scenario the decision attached, kept where the callback can reach it.
   One per case, which is one per process at a time: the cases run in
   sequence. */
static ScenarioHost *scSlot;

static ServerSim *scSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimSetState(sim, serverStateLobby);

    /* Before the commit, because a script that prints at its top level
       prints while the attach is still running. The cases that read nothing
       are not disturbed by it: the watcher passes every line on and keeps a
       copy of the marked ones. */
    scNoteReset();
    scConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = scConsoleCb;

    scSlot = NULL;
    serverSimSetScenarioDir(sim, scDir);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &scSlot);
    return sim;
}

static void scDestroy(ServerSim *sim) {
    /* The host holds registrations on the sim and a subscriber slot in it,
       so it goes before the sim does. */
    scenarioHostDetach(scSlot);
    scSlot = NULL;
    sim->sim.callbacks.consoleMessage = scConsolePrev;
    scConsolePrev = NULL;
    serverSimDestroy(sim);
}

/* A map commit, the way the sim makes it after loading the new map. */
static void scCommit(ServerSim *sim) {
    const char *mapPath = scMapPath();

    SDL_strlcpy(sim->mapFilePath, mapPath, sizeof(sim->mapFilePath));
    serverSimScenarioOnMapChanged(sim, mapPath);
    serverSimScenarioApplyLobbyRules(sim);
}

/* The host setting its whole list, through the command the lobby sends. The
   pick cooldown is one list a second, and these cases send several. */
static CmdResult scPick(ServerSim *sim, const char *const *files, int count) {
    ClientCommand cmd;
    CmdResult     r;
    int           i;

    sim->scenarioPickTick = 0;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 1;
    cmd.u.setScriptList.count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        snprintf(cmd.u.setScriptList.files[i],
                 sizeof(cmd.u.setScriptList.files[i]), "%s", files[i]);
    }
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* A map commit, a pick and a round start, which is the whole of what a case
   below needs before it can drive a site. NULL when any step of it was
   refused, and the caller's assert on that is what names the step.

   The round start is the in-place one. It boots the round's own VM, runs
   every script's chunk in it, resolves the hooks, builds a tank and a
   builder for the seat and calls on_setup, and it does all of that without
   reloading the map — which these cases need, because the map they name was
   never written to disk. serverSimStartGame reloads it and would find
   nothing there.

   The lobby is switched off after the pick and not before it: the pick goes
   in through the lobby command, and what follows it is a round. */
static ServerSim *scRunningSim(const char *const *picks, int count) {
    ServerSim *sim = scSim();

    if (sim == NULL) {
        return NULL;
    }
    scCommit(sim);
    if (count > 0 && scPick(sim, picks, count) != CMD_OK) {
        scDestroy(sim);
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGameInPlace(sim);
    return sim;
}

/* Seats the composed list put on one team. */
static int scSeats(const ServerSim *sim, BYTE team) {
    int n = 0, i;

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == team) {
            n++;
        }
    }
    return n;
}

/* Whether the composite holds a region under this name. */
static bool scHasRegion(const ScenarioManifest *m, const char *name) {
    uint8_t i;

    if (m == NULL) return false;
    for (i = 0; i < m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) return true;
    }
    return false;
}

/* The bit one named region of the composite answers to, or -1 for a name it
   does not hold. The bit is the region's identity for the round: the host's
   per-seat mask is indexed by it, so two composes that give one region two
   different bits are two rounds in which a tank standing still would be
   reported as having walked out of one region and into another. */
static int scRegionBit(const ScenarioManifest *m, const char *name) {
    uint8_t i;

    if (m == NULL) return -1;
    for (i = 0; i < m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) {
            return (int)m->regions[i].bit;
        }
    }
    return -1;
}

/* Whether the composite holds this tag on this pill. */
static bool scPillTagged(const ScenarioManifest *m, int pill,
                         const char *tag) {
    uint8_t i;

    if (m == NULL || pill < 1 || pill > MAX_PILLS) return false;
    for (i = 0; i < m->pillTags[pill].count; i++) {
        if (strcmp(m->pillTags[pill].tag[i], tag) == 0) return true;
    }
    return false;
}

/* ── 1. A map's scenario and two mods play together ───────────────── */

int run_scenario_compose_map_and_mods(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const char             *picks[2] = { "modone.lua", "modtwo.lua" };

    UT_ASSERT(scMakeDir("map_and_mods"));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scWrite("modtwo.lua", kScModTwo));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT_MSG(scPick(sim, picks, 2) == CMD_OK, "the two mods were refused");
    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));

    /* Three scripts, not one: the map's own and both picks. */
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted 3",
                  scenarioHostScriptCount(scSlot));

    /* The declarative half is the map's scenario's, because that is the one
       script on the list that decides the round. */
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(m->name, "Survival") == 0,
                  "the composite is named \"%s\", wanted the scenario's",
                  m->name);
    UT_ASSERT_MSG(strcmp(m->game, "tournament") == 0,
                  "the composite plays \"%s\", wanted the scenario's game",
                  m->game);
    UT_ASSERT_MSG(scSeats(sim, 2) == 2,
                  "the scenario's seats read %d, wanted its two",
                  scSeats(sim, 2));

    /* And the list halves are every script's. */
    UT_ASSERT_MSG(m->numRegions == 3,
                  "the composite holds %d regions, wanted three",
                  (int)m->numRegions);
    UT_ASSERT_MSG(scHasRegion(m, "north"), "the map's own region is missing");
    UT_ASSERT_MSG(scHasRegion(m, "south"), "the first mod's region is missing");
    UT_ASSERT_MSG(scHasRegion(m, "east"), "the second mod's region is missing");
    /* The map's own region is first, because the map's own script is first
       on the list and the array is packed in list order. What a region is
       known by is its bit and not this index, which is what
       scenario_compose_map_script_placed below holds to. */
    UT_ASSERT_MSG(strcmp(m->regions[0].name, "north") == 0,
                  "region 0 is \"%s\", wanted the map's own",
                  m->regions[0].name);

    /* Two scripts tagging one pill is two tags. */
    UT_ASSERT_MSG(scPillTagged(m, 1, "home"),
                  "the map's own tag on pill 1 is missing");
    UT_ASSERT_MSG(scPillTagged(m, 1, "watched"),
                  "the mod's tag on pill 1 is missing");
    UT_ASSERT_MSG(scPillTagged(m, 2, "outer"),
                  "the first mod's tag on pill 2 is missing");

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 2. A mod does not take the map's scenario off, a scenario does ── */

int run_scenario_compose_keeps_map_scenario(void) {
    ServerSim         *sim;
    const ScnDirEntry *row;
    const char        *mod[1]   = { "modone.lua" };
    const char        *other[1] = { "duel.lua" };

    UT_ASSERT(scMakeDir("keeps_map"));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    /* The map alone: one script, and the published list is its row. */
    UT_ASSERT(scSlot != NULL);
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 1,
                  "the map alone published %d rows, wanted one",
                  serverSimGetLobbyScriptCount(sim));
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->bound,
                  "the map's own row is not marked bound, so a chooser would "
                  "offer to take it off");

    /* And with a mod picked the scenario is still the map's. */
    UT_ASSERT_MSG(scPick(sim, mod, 1) == CMD_OK, "the mod was refused");
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioMap,
                  "a picked mod left the source at %d, wanted map (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Survival") == 0,
                  "a picked mod renamed the round \"%s\"",
                  sim->scenarioIdentity.name);
    UT_ASSERT_MSG(scSeats(sim, 2) == 2,
                  "the map's seats read %d behind a mod, wanted two",
                  scSeats(sim, 2));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "%d scripts loaded, wanted the map's and the mod",
                  scenarioHostScriptCount(scSlot));

    /* The published list is both, the map's first and bound, the pick behind
       it and not bound: what the host may take off is exactly the second
       row. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "the lobby was published %d rows, wanted two",
                  serverSimGetLobbyScriptCount(sim));
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL && row->bound);
    UT_ASSERT_MSG(strcmp(row->name, "Survival") == 0,
                  "row 0 is \"%s\", wanted the map's own", row->name);
    row = serverSimGetLobbyScript(sim, 1);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(!row->bound, "the host's own pick came back bound");
    UT_ASSERT_MSG(strcmp(row->file, "modone.lua") == 0,
                  "row 1 is \"%s\", wanted the pick", row->file);
    /* The picks on their own are what the host may edit, and the map's row is
       not one of them. */
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 1,
                  "the host's own list holds %d, wanted its one pick",
                  serverSimGetScriptCount(sim));

    /* A picked scenario is the other half of the rule: it replaces the map's
       own, because two scripts cannot both decide the round. */
    UT_ASSERT_MSG(scPick(sim, other, 1) == CMD_OK, "the scenario was refused");
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioMod,
                  "a picked scenario left the source at %d, wanted mod (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioMod);
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Duel") == 0,
                  "the picked scenario read back as \"%s\"",
                  sim->scenarioIdentity.name);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 1,
                  "%d scripts loaded, wanted the pick alone",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 1,
                  "the lobby was published %d rows, wanted the pick alone",
                  serverSimGetLobbyScriptCount(sim));
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "the map's row outlived the scenario that replaced it");

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* What the composite sets one rule to, and false when its table does not name
   that rule at all. The two answers are different: a rule nobody set is the
   classic value, and a rule set to the classic value is a row of the table.
   */
static bool scRuleValue(const ScenarioManifest *m, int rule, double *out) {
    uint16_t i;

    if (m == NULL) return false;
    for (i = 0; i < m->numRules; i++) {
        if ((int)m->rules[i].rule == rule) {
            *out = m->rules[i].value;
            return true;
        }
    }
    return false;
}

/* How many of the composite's regions carry this name. Two scripts naming
   one region is two rectangles and not one, so the cases below count rather
   than ask whether the name is there. */
static int scRegionCount(const ScenarioManifest *m, const char *name) {
    int     n = 0;
    uint8_t i;

    if (m == NULL) return 0;
    for (i = 0; i < m->numRegions; i++) {
        if (strcmp(m->regions[i].name, name) == 0) n++;
    }
    return n;
}

/* ── 3. A mod that declares a rules block plays ───────────────────── */

int run_scenario_compose_mod_rules_load(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const char             *picks[1] = { "modrules.lua" };
    double                  value    = 0.0;

    UT_ASSERT(scMakeDir("mod_rules"));
    UT_ASSERT(scWrite("modrules.lua", kScModRules));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT(scSlot != NULL);

    /* This used to be the refusal case: a mod with a rules block in it took
       the whole list down. It now plays, and the map's own scenario is still
       the one deciding the round. */
    UT_ASSERT(scPick(sim, picks, 1) == CMD_OK);
    UT_ASSERT_MSG(scSlot != NULL, "a mod with a rules block took the list "
                  "down: %s", scenarioHostLastError(scSlot));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the map's and the "
                  "mod", scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioMap,
                  "the source reads %d, wanted map (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioMap);
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "%d rows published, wanted the map's and the pick",
                  serverSimGetLobbyScriptCount(sim));
    UT_ASSERT_MSG(serverSimGetMapScript(sim) != NULL,
                  "the map's row is missing from a round that is playing it");

    /* And the mod's rule is in the composite, which is what the round
       applies at its start. */
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(scRuleValue(m, (int)SIM_RULE_tank_death_ticks, &value),
                  "the composite names %d rules and none of them is the "
                  "mod's", (int)m->numRules);
    UT_ASSERT_MSG(value == 400.0,
                  "the composite sets tank_death_ticks to %g, wanted the "
                  "mod's 400", value);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 4. Two scripts naming one region ─────────────────────────────── */

int run_scenario_compose_region_clash_loads(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const char             *picks[1] = { "modclash.lua" };

    UT_ASSERT(scMakeDir("region_clash"));
    UT_ASSERT(scWrite("modclash.lua", kScModClash));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT(scSlot != NULL);
    UT_ASSERT(scPick(sim, picks, 1) == CMD_OK);
    /* This used to be the region refusal case. Both rectangles are kept now,
       one per script, and neither file has to be edited because the other
       one happened to pick the same word. */
    UT_ASSERT_MSG(scSlot != NULL,
                  "two scripts named one region and the list was refused");
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(scRegionCount(m, "north") == 2,
                  "the composite holds %d regions called north, wanted one "
                  "per script", scRegionCount(m, "north"));
    /* Each carries the script that named it, which is what every lookup by
       name is answered from. The map's own script is entry 0 and the pick is
       entry 1, and an owner is a list position plus one. */
    UT_ASSERT_MSG(m->regions[0].owner == 1 && m->regions[1].owner == 2,
                  "the two regions read back owned by %d and %d, wanted the "
                  "map's script and the pick",
                  (int)m->regions[0].owner, (int)m->regions[1].owner);
    UT_ASSERT_MSG(m->regions[0].x == 10 && m->regions[1].x == 60,
                  "the rectangles read back at x=%d and x=%d, wanted the "
                  "map's 10 and the pick's 60",
                  (int)m->regions[0].x, (int)m->regions[1].x);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 5. A reload re-reads the whole list ──────────────────────────── */

int run_scenario_compose_reload_list(void) {
    ServerSim  *sim;
    const char *picks[2] = { "modone.lua", "modtwo.lua" };
    char        err[512];

    UT_ASSERT(scMakeDir("reload_list"));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scWrite("modtwo.lua", kScModTwo));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT(scPick(sim, picks, 2) == CMD_OK);
    UT_ASSERT(scSlot != NULL);
    UT_ASSERT(scenarioHostScriptCount(scSlot) == 3);

    /* One script on the list edited on disk, and the reload takes it. */
    UT_ASSERT(scWrite("modtwo.lua", kScModTwoEdited));
    err[0] = '\0';
    threadsWaitForMutex();
    UT_ASSERT_MSG(scenarioHostReload(scSlot, err, sizeof(err)),
                  "the reload was refused: %s", err);
    threadsReleaseMutex();
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the reload left %d scripts on the list, wanted three",
                  scenarioHostScriptCount(scSlot));
    /* The scenario is still the map's, which is what a reload of a list has
       to leave alone. */
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Survival") == 0,
                  "the reload renamed the round \"%s\"",
                  sim->scenarioIdentity.name);

    /* And a file with an error in it is refused with the list left as it
       was: the round that is running keeps its three scripts. */
    UT_ASSERT(scWrite("modtwo.lua", kScModTwoBroken));
    err[0] = '\0';
    threadsWaitForMutex();
    UT_ASSERT_MSG(!scenarioHostReload(scSlot, err, sizeof(err)),
                  "a broken script reloaded anyway");
    threadsReleaseMutex();
    UT_ASSERT_MSG(err[0] != '\0', "the refusal said nothing");
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "a refused reload left %d scripts on the list",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Survival") == 0,
                  "a refused reload changed the round to \"%s\"",
                  sim->scenarioIdentity.name);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── The scripts the round cases compose ──────────────────────────── */

/* What a script of these cases writes down: the tag it was asked under, and
   either "ok" for a row that answered or the raise it got back. A raise is
   how the guard refuses, so the text of one is the thing being read.

   pcall is in the sandbox, so a script can catch its own refusal; the
   sandbox wraps it and re-raises only an instruction-budget stop, which
   none of these calls comes near. */
#define SC_TRY                                                               \
    "local function try(tag, fn, a, b)\n"                                    \
    "  local ok, err = pcall(fn, a, b)\n"                                    \
    "  if ok then\n"                                                         \
    "    print(\"note:\" .. tag .. \"=ok;\")\n"                              \
    "  else\n"                                                               \
    "    print(\"note:\" .. tag .. \"=\" .. tostring(err) .. \";\")\n"       \
    "  end\n"                                                                \
    "end\n"

/* The three rows the guard holds, called from the script that decides the
   round. Every one of them has to go through.

   The arguments are chosen so that nothing the round would notice happens.
   game.end_round is handed a winner_team of 999, which the row itself
   refuses for range and returns from: the refusal is a returned value and
   not a raise, so "ok" still says the guard let the call in, and the round
   this case is still using does not end underneath it. The guard is asked
   ahead of the arguments either way, which is why a mod hitting the same
   call gets the raise instead. */
static const char kScGuardBase[] =
    "scenario = {\n"
    "  name = \"Guard Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    SC_TRY
    "function on_setup()\n"
    "  try(\"base_score\", game.score, 0, 5)\n"
    "  try(\"base_time\", game.set_game_time, 3600)\n"
    "  try(\"base_end\", game.end_round, \"x\", 999)\n"
    "end\n";

/* And the same three from a mod on the same list, which is the defect this
   case is here for: the mod has to be refused although the composite it is
   composed into carries the base's kind. */
static const char kScGuardMod[] =
    "scenario = {\n"
    "  name = \"Guard Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    SC_TRY
    "function on_setup()\n"
    "  try(\"mod_score\", game.score, 0, 5)\n"
    "  try(\"mod_time\", game.set_game_time, 3600)\n"
    "  try(\"mod_end\", game.end_round, \"x\", 999)\n"
    "end\n";

/* Two mods handing a spawning tank different stores. A tank spawns with one
   set of stores, so these two answers cannot be added up and one of them has
   to win. The numbers are picked apart from each other and from anything a
   game type hands out, so the amounts read back name the script they came
   from. */
static const char kScStoresOne[] =
    "scenario = {\n"
    "  name = \"Stores One\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function spawn_loadout(player)\n"
    "  return { shells = 11, mines = 12, armour = 13, trees = 14 }\n"
    "end\n";

static const char kScStoresTwo[] =
    "scenario = {\n"
    "  name = \"Stores Two\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function spawn_loadout(player)\n"
    "  return { shells = 21, mines = 22, armour = 23, trees = 24 }\n"
    "end\n";

/* A base that answers no policy at all, so what the round answers is
   whatever the mods behind it say. */
static const char kScQuietBase[] =
    "scenario = {\n"
    "  name = \"Quiet Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n";

/* data/mods/NoLgmDeaths.scenario.lua, which is the mod the composed round
   has to ask: its whole behaviour is this one policy, and a policy-only mod
   that is loaded and never asked does nothing at all. */
static const char kScNoLgmDeaths[] =
    "scenario = {\n"
    "  name = \"No LGM Deaths\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function can_die(kind, n, killer, cause)\n"
    "  print(\"note:no_asked;\")\n"
    "  if kind == \"builder\" then\n"
    "    return false\n"
    "  end\n"
    "  return true\n"
    "end\n";

/* A mod that answers the same policy yes, and says it was asked. Behind the
   one above it is never reached; ahead of it, it is. */
static const char kScDieYes[] =
    "scenario = {\n"
    "  name = \"Deaths As Usual\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function can_die(kind, n, killer, cause)\n"
    "  print(\"note:yes_asked;\")\n"
    "  return true\n"
    "end\n";

/* Two mods halving a blow. Two files with two names, because the list
   refuses the same file twice and this case needs two scripts answering one
   row of the policy. */
static const char kScHalfOne[] =
    "scenario = {\n"
    "  name = \"Half One\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function damage_scale(attacker, victim, cause)\n"
    "  return 50\n"
    "end\n";

static const char kScHalfTwo[] =
    "scenario = {\n"
    "  name = \"Half Two\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function damage_scale(attacker, victim, cause)\n"
    "  return 50\n"
    "end\n";

/* A base that takes the base sweep out of the round, and says it was asked
   to. */
static const char kScSweepOffBase[] =
    "scenario = {\n"
    "  name = \"Sweep Off\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    "function allow_base_win()\n"
    "  print(\"note:base_asked;\")\n"
    "  return false\n"
    "end\n";

/* A mod that writes the same function. It is the one policy the list is not
   asked, so this must never print. */
static const char kScSweepMod[] =
    "scenario = {\n"
    "  name = \"Sweep Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function allow_base_win()\n"
    "  print(\"note:mod_asked;\")\n"
    "  return false\n"
    "end\n";

/* ── The square a builder can be killed on ────────────────────────── */

/* Not a building, not under a pillbox and not under a base, which are the
   three the death test reads as solid. Searched rather than named: the case
   is about what the host answers, not about where on Everard Island the
   blast lands, and a named square would have to be checked again every time
   the shipped map changed. */
static bool scOpenSquare(GameSim *gs, BYTE *outX, BYTE *outY) {
    int x;
    int y;

    for (y = 20; y < 200; y++) {
        for (x = 20; x < 200; x++) {
            BYTE pos = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);

            if (pos == BUILDING || pos == HALFBUILDING) {
                continue;
            }
            if (pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y)) {
                continue;
            }
            if (basesExistPos(&gs->bs, (BYTE)x, (BYTE)y)) {
                continue;
            }
            *outX = (BYTE)x;
            *outY = (BYTE)y;
            return true;
        }
    }
    return false;
}

/* A blast on the builder of slot 0, and whether it killed him.

   lgmDeathCheck is the one site every builder death in the engine comes
   through, and the host's say is the gameSimCanDie inside it — see
   src/bolo/lgm.c:1520. The man is placed by hand and stood out of his tank
   by hand: walking him out takes a round's worth of ticks and decides
   nothing this case is about, and the death test does nothing at all for a
   man who is still inside. */
static bool scBlastKillsBuilder(ServerSim *sim, BYTE mx, BYTE my) {
    GameSim *gs = serverSimGetGameSim(sim);
    WORLD    wx = (WORLD)(((WORLD)mx << 8) + 128);
    WORLD    wy = (WORLD)(((WORLD)my << 8) + 128);

    gs->lgmen[0]->inTank = FALSE;
    gs->lgmen[0]->isDead = FALSE;
    gs->lgmen[0]->x      = wx;
    gs->lgmen[0]->y      = wy;
    lgmDeathCheck(gs, &gs->lgmen[0], wx, wy, NEUTRAL, NEUTRAL, DMG_SRC_UNKNOWN,
                  DMG_NO_PILL, &gs->tanks[0]);
    return gs->lgmen[0]->isDead != FALSE;
}

/* The round built a tank and a builder for slot 0, which every case below
   that drives an engine site needs before it can drive one. */
static bool scHasCrew(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);

    return gs != NULL && gs->tanks[0] != NULL && gs->lgmen[0] != NULL;
}

/* ── 6. A mod behind a scenario may not decide the round ──────────── */

int run_scenario_compose_mod_guard(void) {
    ServerSim  *sim;
    const char *picks[1] = { "guardmod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("mod_guard"));
    UT_ASSERT(scWrite("guardmod.lua", kScGuardMod));
    UT_ASSERT(scPutMapScript(kScGuardBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    /* The base decides the round, so all three rows are its to call. */
    UT_ASSERT_MSG(strstr(said, "base_score=ok;") != NULL,
                  "the base scenario was refused game.score: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_time=ok;") != NULL,
                  "the base scenario was refused game.set_game_time: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_end=ok;") != NULL,
                  "the base scenario was refused game.end_round: %s", said);

    /* And the mod on the same list is refused all three, each refusal naming
       the row it turned down. What this pins is that the guard reads the
       script that is calling and not the composite: a mod is composed into a
       manifest whose kind came from the base, so a guard reading the
       composite lets the mod through. */
    UT_ASSERT_MSG(strstr(said, "game.score decides the round") != NULL,
                  "the mod called game.score and was not refused: %s", said);
    UT_ASSERT_MSG(strstr(said, "game.set_game_time decides the round") != NULL,
                  "the mod called game.set_game_time and was not refused: %s",
                  said);
    UT_ASSERT_MSG(strstr(said, "game.end_round decides the round") != NULL,
                  "the mod called game.end_round and was not refused: %s",
                  said);
    /* The tags are there too, so the three lines above are the mod's own and
       not one refusal read three times. */
    UT_ASSERT_MSG(strstr(said, "mod_score=") != NULL &&
                      strstr(said, "mod_time=") != NULL &&
                      strstr(said, "mod_end=") != NULL,
                  "the mod's own on_setup did not run: %s", said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 7. A policy-only mod behind a scenario is asked ──────────────── */

int run_scenario_compose_policy_mod_asked(void) {
    ServerSim  *sim;
    const char *picks[1] = { "nolgm.lua" };
    BYTE        mx = 0;
    BYTE        my = 0;

    UT_ASSERT(scMakeDir("policy_mod_asked"));
    UT_ASSERT(scWrite("nolgm.lua", kScNoLgmDeaths));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* The mod on the list: the builder lives through the blast, because the
       round asked the one script that had anything to say about it. */
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(scHasCrew(sim), "the round built no tank or builder");
    UT_ASSERT(scOpenSquare(serverSimGetGameSim(sim), &mx, &my));
    UT_ASSERT_MSG(!scBlastKillsBuilder(sim, mx, my),
                  "the builder died although a mod on the list refused it");
    scDestroy(sim);

    /* And the same blast with the mod off the list kills him, so what the
       assert above read is the mod's answer and not the square. */
    sim = scRunningSim(NULL, 0);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 1,
                  "the round composed %d scripts, wanted the base alone",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(scHasCrew(sim), "the round built no tank or builder");
    UT_ASSERT(scOpenSquare(serverSimGetGameSim(sim), &mx, &my));
    UT_ASSERT_MSG(scBlastKillsBuilder(sim, mx, my),
                  "the builder lived through a blast with nothing on the list "
                  "to refuse it");
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 8. Two scripts on one predicate: any false wins ──────────────── */

int run_scenario_compose_policy_any_false(void) {
    ServerSim  *sim;
    const char *noFirst[2]  = { "nolgm.lua", "dieyes.lua" };
    const char *yesFirst[2] = { "dieyes.lua", "nolgm.lua" };
    BYTE        mx = 0;
    BYTE        my = 0;
    char        said[8192];

    UT_ASSERT(scMakeDir("policy_any_false"));
    UT_ASSERT(scWrite("nolgm.lua", kScNoLgmDeaths));
    UT_ASSERT(scWrite("dieyes.lua", kScDieYes));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* The no first. It wins, and the yes behind it is never asked: a script
       that has already lost has nothing left to add, so the asking stops at
       the first no. */
    sim = scRunningSim(noFirst, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scHasCrew(sim));
    UT_ASSERT(scOpenSquare(serverSimGetGameSim(sim), &mx, &my));
    scNoteReset();
    UT_ASSERT_MSG(!scBlastKillsBuilder(sim, mx, my),
                  "the builder died although the first mod refused it");
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "no_asked;") != NULL,
                  "the mod that refuses was not asked: %s", said);
    UT_ASSERT_MSG(strstr(said, "yes_asked;") == NULL,
                  "the asking went on past a no: %s", said);
    scDestroy(sim);

    /* The yes first. Both are asked, and the answer is the same: the list
       order does not decide a predicate. */
    sim = scRunningSim(yesFirst, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(scHasCrew(sim));
    UT_ASSERT(scOpenSquare(serverSimGetGameSim(sim), &mx, &my));
    scNoteReset();
    UT_ASSERT_MSG(!scBlastKillsBuilder(sim, mx, my),
                  "the list order decided the answer: the builder died with "
                  "the refusing mod second");
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "yes_asked;") != NULL,
                  "the first mod was not asked: %s", said);
    UT_ASSERT_MSG(strstr(said, "no_asked;") != NULL,
                  "the second mod was not asked: %s", said);
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 9. Two scripts halving a blow leave a quarter of it ──────────── */

int run_scenario_compose_policy_damage_scale(void) {
    ServerSim  *sim;
    const char *both[2] = { "halfone.lua", "halftwo.lua" };
    const char *one[1]  = { "halfone.lua" };
    int         amount;

    UT_ASSERT(scMakeDir("policy_damage"));
    UT_ASSERT(scWrite("halfone.lua", kScHalfOne));
    UT_ASSERT(scWrite("halftwo.lua", kScHalfTwo));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* A blow priced with nothing on the list to change it: the classic
       amount, which is what the two below are read against. The attacker and
       the victim are NEUTRAL so that no tank's own damage modifiers take
       part and the policy is the only factor left. */
    sim = scRunningSim(NULL, 0);
    UT_ASSERT(sim != NULL);
    amount = (int)tankDamageAmount(serverSimGetGameSim(sim), 100, NEUTRAL,
                                   NEUTRAL, LAST_DEATH_BY_SHELL, DMG_NO_PILL);
    UT_ASSERT_MSG(amount == 100,
                  "a blow of 100 with no script on the list came to %d",
                  amount);
    scDestroy(sim);

    /* One script halving it, which has to read exactly as it did before
       there were lists. */
    sim = scRunningSim(one, 1);
    UT_ASSERT(sim != NULL);
    amount = (int)tankDamageAmount(serverSimGetGameSim(sim), 100, NEUTRAL,
                                   NEUTRAL, LAST_DEATH_BY_SHELL, DMG_NO_PILL);
    UT_ASSERT_MSG(amount == 50,
                  "one script halving a blow of 100 left %d", amount);
    scDestroy(sim);

    /* And two of them leave a quarter: the percents multiply, so each
       script's answer still means on a list what it means alone. */
    sim = scRunningSim(both, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted the base and two mods",
                  scenarioHostScriptCount(scSlot));
    amount = (int)tankDamageAmount(serverSimGetGameSim(sim), 100, NEUTRAL,
                                   NEUTRAL, LAST_DEATH_BY_SHELL, DMG_NO_PILL);
    UT_ASSERT_MSG(amount == 25,
                  "two scripts halving a blow of 100 left %d, wanted a "
                  "quarter", amount);
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 10. allow_base_win is the base's alone ───────────────────────── */

int run_scenario_compose_policy_base_win(void) {
    ServerSim  *sim;
    const char *picks[1] = { "sweepmod.lua" };
    const char *noSweep[1] = { "halftwo.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("policy_base_win"));
    UT_ASSERT(scWrite("sweepmod.lua", kScSweepMod));
    UT_ASSERT(scPutMapScript(kScSweepOffBase));

    /* The base's own no takes the sweep out of the round, and the mod's copy
       of the same function is not read at all. */
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scNoteReset();
    UT_ASSERT_MSG(serverSimCheckGameWin(sim, false) == FALSE,
                  "the sweep ended a round whose scenario refused it");
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "base_asked;") != NULL,
                  "the base scenario's allow_base_win was not asked: %s", said);
    UT_ASSERT_MSG(strstr(said, "mod_asked;") == NULL,
                  "a mod's allow_base_win was asked: %s", said);
    scDestroy(sim);

    /* And with the base saying nothing, the mod's no is still not read: the
       one policy that decides an ending is not asked of the list. */
    UT_ASSERT(scPutMapScript(kScQuietBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    scNoteReset();
    (void)serverSimCheckGameWin(sim, false);
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "mod_asked;") == NULL,
                  "a mod answered allow_base_win for a base that said "
                  "nothing: %s", said);
    scDestroy(sim);

    /* With no map script the one picked mod is the base, and the host says
       once that its allow_base_win is skipped — but only when it wrote one.
       A mod that never did is not told about a function it does not have. */
    scDropMapScript();
    UT_ASSERT(scWrite("halftwo.lua", kScHalfTwo));
    sim = scRunningSim(noSweep, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 1,
                  "the round composed %d scripts, wanted the mod alone",
                  scenarioHostScriptCount(scSlot));
    scNoteReset();
    (void)serverSimCheckGameWin(sim, false);
    (void)serverSimCheckGameWin(sim, false);
    UT_ASSERT_MSG(scBaseWinSkipped == 0,
                  "a mod with no allow_base_win was told %d times that its "
                  "allow_base_win is not read", scBaseWinSkipped);
    scDestroy(sim);

    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 1,
                  "the round composed %d scripts, wanted the mod alone",
                  scenarioHostScriptCount(scSlot));
    scNoteReset();
    (void)serverSimCheckGameWin(sim, false);
    (void)serverSimCheckGameWin(sim, false);
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(scBaseWinSkipped == 1,
                  "a mod that wrote allow_base_win was told %d times that it "
                  "is not read, wanted once", scBaseWinSkipped);
    UT_ASSERT_MSG(strstr(said, "mod_asked;") == NULL,
                  "a mod answered allow_base_win with no base: %s", said);
    scDestroy(sim);

    scDropDir();
    return 0;
}

/* ── 11. Two scripts on one loadout: the later one wins ───────────── */

int run_scenario_compose_policy_first_answer(void) {
    ServerSim  *sim;
    const char *oneThenTwo[2] = { "storesone.lua", "storestwo.lua" };
    const char *twoThenOne[2] = { "storestwo.lua", "storesone.lua" };
    BYTE        shells = 0;
    BYTE        mines  = 0;
    BYTE        armour = 0;
    BYTE        trees  = 0;

    UT_ASSERT(scMakeDir("policy_first_answer"));
    UT_ASSERT(scWrite("storesone.lua", kScStoresOne));
    UT_ASSERT(scWrite("storestwo.lua", kScStoresTwo));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* Both mods answer, and the one further up the list is the one the tank
       is handed. The list order is the thing a host sets deliberately, and
       the chooser says the top has priority. */
    sim = scRunningSim(oneThenTwo, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted the base and two mods",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(gameSimSpawnLoadout(serverSimGetGameSim(sim), 0, &shells,
                                      &mines, &armour, &trees),
                  "no script answered spawn_loadout");
    UT_ASSERT_MSG(shells == 11 && mines == 12 && armour == 13 && trees == 14,
                  "the tank was handed %d/%d/%d/%d, wanted the earlier "
                  "script's 11/12/13/14", (int)shells, (int)mines,
                  (int)armour, (int)trees);
    scDestroy(sim);

    /* The same two files the other way round give the other answer, so what
       the assert above read is the list order and not the file. */
    sim = scRunningSim(twoThenOne, 2);
    UT_ASSERT(sim != NULL);
    shells = 0;
    mines  = 0;
    armour = 0;
    trees  = 0;
    UT_ASSERT_MSG(gameSimSpawnLoadout(serverSimGetGameSim(sim), 0, &shells,
                                      &mines, &armour, &trees),
                  "no script answered spawn_loadout with the list flipped");
    UT_ASSERT_MSG(shells == 21 && mines == 22 && armour == 23 && trees == 24,
                  "with the list flipped the tank was handed %d/%d/%d/%d, "
                  "wanted 21/22/23/24", (int)shells, (int)mines, (int)armour,
                  (int)trees);
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── The scripts the list-order cases compose ─────────────────────── */

/* Two mods setting one rule, and one of them setting a second rule the other
 * says nothing about. The reload interval is the rule they share: classic is
 * 13, so neither 5 nor 9 can be read as the value nobody set. The shell
 * stock is the one only the first sets, and classic is 40. */
static const char kScRulesFast[] =
    "scenario = {\n"
    "  name = \"Rules Fast\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  rules = { tank_reload_ticks = 5, tank_full_shells = 80 },\n"
    "}\n";

static const char kScRulesSlow[] =
    "scenario = {\n"
    "  name = \"Rules Slow\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  rules = { tank_reload_ticks = 9 },\n"
    "}\n";

/* Two scripts naming one region under rectangles far enough apart that the x
 * alone says which one a lookup answered with. Each reads the name back at
 * its setup, which is the "its own first" half of the lookup rule. */
static const char kScZoneBase[] =
    "scenario = {\n"
    "  name = \"Zone Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  regions = { zone = { x = 10, y = 10, w = 20, h = 20 } },\n"
    "}\n"
    "function on_setup()\n"
    "  local r = game.region(\"zone\")\n"
    "  print(\"note:base_zone=\" .. tostring(r and r.x) .. \";\")\n"
    "end\n";

static const char kScZoneMod[] =
    "scenario = {\n"
    "  name = \"Zone Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  regions = { zone = { x = 60, y = 60, w = 4, h = 4 } },\n"
    "}\n"
    "function on_setup()\n"
    "  local r = game.region(\"zone\")\n"
    "  print(\"note:mod_zone=\" .. tostring(r and r.x) .. \";\")\n"
    "end\n";

/* And a script that names no region at all and reads the same name, which is
 * the other half of the rule: the last one on the list. */
static const char kScZoneReader[] =
    "scenario = {\n"
    "  name = \"Zone Reader\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function on_setup()\n"
    "  local r = game.region(\"zone\")\n"
    "  print(\"note:read_zone=\" .. tostring(r and r.x) .. \";\")\n"
    "end\n";

/* The same three for the hook, with both rectangles covering the whole map.
 * Where the tank is placed then decides nothing: every region of the round
 * holds it, so what the record counts is who was told and not who was
 * standing where. */
#define SC_WHOLE_MAP "{ x = 0, y = 0, w = 255, h = 255 }"

static const char kScHookBase[] =
    "scenario = {\n"
    "  name = \"Hook Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  regions = { zone = " SC_WHOLE_MAP " },\n"
    "}\n"
    "function on_enter_region(p, name)\n"
    "  print(\"note:base_enter=\" .. name .. \";\")\n"
    "end\n";

static const char kScHookMod[] =
    "scenario = {\n"
    "  name = \"Hook Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  regions = { zone = " SC_WHOLE_MAP " },\n"
    "}\n"
    "function on_enter_region(p, name)\n"
    "  print(\"note:mod_enter=\" .. name .. \";\")\n"
    "end\n";

static const char kScHookReader[] =
    "scenario = {\n"
    "  name = \"Hook Reader\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "function on_enter_region(p, name)\n"
    "  print(\"note:read_enter=\" .. name .. \";\")\n"
    "end\n";

/* A pair that clashes on both halves at once: one rule and one region name.
 * The record has to hold two rows and name the same winner and loser in
 * both. */
static const char kScBothBase[] =
    "scenario = {\n"
    "  name = \"Both Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "  rules = { tank_reload_ticks = 5 },\n"
    "  regions = { north = { x = 10, y = 10, w = 20, h = 20 } },\n"
    "}\n";

static const char kScBothMod[] =
    "scenario = {\n"
    "  name = \"Both Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "  rules = { tank_reload_ticks = 9 },\n"
    "  regions = { north = { x = 60, y = 60, w = 4, h = 4 } },\n"
    "}\n";

/* ── Reading the record back ──────────────────────────────────────── */

/* How many times a line appears in the record. The hook case counts rather
   than finds: a script told about its own region once and a script told
   about two regions both wrote the same line, and only the number tells them
   apart. */
static int scCount(const char *hay, const char *needle) {
    const char *at = hay;
    int         n  = 0;

    while ((at = strstr(at, needle)) != NULL) {
        n++;
        at += strlen(needle);
    }
    return n;
}

/* Tick until slot 0's tank is in the world, and twice more so the scan that
   follows it has run. A tank is all the hook case needs from the round: a
   seat with nobody in it is inside nothing, whatever the rectangles say. */
#define SC_TICK_LIMIT 200

static bool scTickForTank(ServerSim *sim) {
    TankInfo info;
    int      i;

    for (i = 0; i < SC_TICK_LIMIT; i++) {
        if (serverSimGetTankInfo(sim, 0, &info) && info.has_tank) {
            serverSimTick(sim);
            serverSimTick(sim);
            return true;
        }
        serverSimTick(sim);
    }
    return false;
}

/* What the round is actually running one rule at, which is the value the
   round start applied out of the composite. */
static int scLiveRule(ServerSim *sim, int which) {
    GameSim *gs = serverSimGetGameSim(sim);

    if (gs == NULL) return -1;
    return (which == (int)SIM_RULE_tank_reload_ticks)
               ? (int)gs->rules.tank_reload_ticks
               : (int)gs->rules.tank_full_shells;
}

/* ── 12. Two scripts on one rule: the later one wins ──────────────── */

int run_scenario_compose_rules_first_wins(void) {
    ServerSim  *sim;
    const char *fastThenSlow[2] = { "rulesfast.lua", "rulesslow.lua" };
    const char *slowThenFast[2] = { "rulesslow.lua", "rulesfast.lua" };

    UT_ASSERT(scMakeDir("rules_first_wins"));
    UT_ASSERT(scWrite("rulesfast.lua", kScRulesFast));
    UT_ASSERT(scWrite("rulesslow.lua", kScRulesSlow));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* Both mods set the reload interval and both load. The one further up
       the list is the value the round runs, which is the precedence the
       policies and the loadout already follow. */
    sim = scRunningSim(fastThenSlow, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted the base and two "
                  "mods", scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks) == 5,
                  "the round reloads every %d ticks, wanted the earlier "
                  "mod's 5",
                  scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks));
    scDestroy(sim);

    /* The same two files the other way round give the other answer, so what
       the assert above read is the list order and not the file. */
    sim = scRunningSim(slowThenFast, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks) == 9,
                  "with the list flipped the round reloads every %d ticks, "
                  "wanted 9",
                  scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks));
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 13. A rule only the earlier script set survives ──────────────── */

int run_scenario_compose_rules_keeps_earlier(void) {
    ServerSim  *sim;
    const char *picks[2] = { "rulesfast.lua", "rulesslow.lua" };

    UT_ASSERT(scMakeDir("rules_keeps_earlier"));
    UT_ASSERT(scWrite("rulesfast.lua", kScRulesFast));
    UT_ASSERT(scWrite("rulesslow.lua", kScRulesSlow));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* The later mod's rules block holds one key, and it is a key the earlier
       mod already set. The merge is key by key, so the shell stock the
       earlier mod set alone is untouched by it: a whole block that replaced
       the one before it would put the shell stock back to classic without
       either file saying so. */
    sim = scRunningSim(picks, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scLiveRule(sim, (int)SIM_RULE_tank_full_shells) == 80,
                  "the round holds %d shells, wanted the earlier mod's 80; "
                  "classic is 40",
                  scLiveRule(sim, (int)SIM_RULE_tank_full_shells));
    UT_ASSERT_MSG(scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks) == 5,
                  "the round reloads every %d ticks, wanted the earlier "
                  "mod's 5",
                  scLiveRule(sim, (int)SIM_RULE_tank_reload_ticks));
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 14. Each script reads its own region back ────────────────────── */

int run_scenario_compose_region_own_first(void) {
    ServerSim  *sim;
    const char *picks[1] = { "zonemod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("region_own_first"));
    UT_ASSERT(scWrite("zonemod.lua", kScZoneMod));
    UT_ASSERT(scPutMapScript(kScZoneBase));

    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the "
                  "mod", scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    /* Both named the region "zone" and both asked for it by that name. Each
       is answered with the rectangle its own file wrote, which is what makes
       the name a script's own business rather than the list's. */
    UT_ASSERT_MSG(strstr(said, "base_zone=10;") != NULL,
                  "the base read its own region back as something else: %s",
                  said);
    UT_ASSERT_MSG(strstr(said, "mod_zone=60;") != NULL,
                  "the mod read its own region back as something else: %s",
                  said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 15. A script that named none reads the one that exists ───────── */

int run_scenario_compose_region_borrowed(void) {
    ServerSim  *sim;
    const char *alone[1] = { "zoneread.lua" };
    const char *both[2]  = { "zonemod.lua", "zoneread.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("region_borrowed"));
    UT_ASSERT(scWrite("zonemod.lua", kScZoneMod));
    UT_ASSERT(scWrite("zoneread.lua", kScZoneReader));
    UT_ASSERT(scPutMapScript(kScZoneBase));

    /* One region of that name on the list, and the script that named none
       finds it. This is every round written before two scripts could share a
       name, and it has to read the same. */
    sim = scRunningSim(alone, 1);
    UT_ASSERT(sim != NULL);
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "read_zone=10;") != NULL,
                  "a script that named no region did not find the one on the "
                  "list: %s", said);
    scDestroy(sim);

    /* Two of them, and the reader takes the first on the list: the same
       direction the rules and the policies run in, so a host who wants the
       other answer moves the file rather than editing it. The map's own
       script is the first on every list, so its rectangle is the one the
       reader is answered with, and the mod's 60 is what a round that had
       kept the old direction would say. */
    sim = scRunningSim(both, 2);
    UT_ASSERT(sim != NULL);
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "read_zone=10;") != NULL,
                  "with two regions of that name the reader did not take the "
                  "first on the list: %s", said);
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 16. The enter hook stays inside the script that named it ─────── */

int run_scenario_compose_region_hook_own(void) {
    ServerSim  *sim;
    const char *picks[2] = { "hookmod.lua", "hookread.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("region_hook_own"));
    UT_ASSERT(scWrite("hookmod.lua", kScHookMod));
    UT_ASSERT(scWrite("hookread.lua", kScHookReader));
    UT_ASSERT(scPutMapScript(kScHookBase));

    sim = scRunningSim(picks, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted the base and two "
                  "mods", scenarioHostScriptCount(scSlot));

    /* The setup lines are not this case's, and the tank is not in the world
       until the placement puts it there. The record starts at the tick the
       scan runs on. */
    scNoteReset();
    UT_ASSERT_MSG(scTickForTank(sim), "slot 0 never got a tank");
    scReadNote(said, sizeof(said));

    /* Two regions called "zone", both covering the map, and one tank inside
       both of them. The base and the mod each named one, so each is told
       about its own and not about the other's: one line apiece and not
       two. */
    UT_ASSERT_MSG(scCount(said, "base_enter=zone;") == 1,
                  "the base heard %d entries into \"zone\", wanted its own "
                  "region alone; the record was:\n%s",
                  scCount(said, "base_enter=zone;"), said);
    UT_ASSERT_MSG(scCount(said, "mod_enter=zone;") == 1,
                  "the mod heard %d entries into \"zone\", wanted its own "
                  "region alone; the record was:\n%s",
                  scCount(said, "mod_enter=zone;"), said);

    /* And the script that named no region of that name hears about both,
       because both are regions it could have asked for and neither is more
       its own than the other. That is the deliberate half of the rule: the
       filter is about a script's own name being taken, and not about cutting
       the round down to one rectangle per name. */
    UT_ASSERT_MSG(scCount(said, "read_enter=zone;") == 2,
                  "the script that named no region heard %d entries into "
                  "\"zone\", wanted both of them; the record was:\n%s",
                  scCount(said, "read_enter=zone;"), said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 17. What the compose wrote down ──────────────────────────────── */

int run_scenario_compose_conflicts_recorded(void) {
    ServerSim                *sim;
    const ScnComposeConflict *row;
    const char               *picks[1] = { "bothmod.lua" };
    char                      mapFile[256];

    UT_ASSERT(scMakeDir("conflicts"));
    UT_ASSERT(scWrite("bothmod.lua", kScBothMod));
    UT_ASSERT(scPutMapScript(kScBothBase));
    /* The leaf, not the path: a conflict row holds the script's own file
       name, which is what scnFileNameOf cut it down to. */
    scScriptFor(SC_MAP_LEAF, mapFile, sizeof(mapFile));

    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the "
                  "mod", scenarioHostScriptCount(scSlot));

    /* One rule set twice and one region name used twice: two rows, in the
       order the compose met them, which is the list's order and the rules
       ahead of the regions inside one script. */
    UT_ASSERT_MSG(scenarioHostConflictCount(scSlot) == 2,
                  "the compose wrote down %d conflicts, wanted the rule and "
                  "the region", scenarioHostConflictCount(scSlot));
    UT_ASSERT_MSG(!scenarioHostConflictsOverflowed(scSlot),
                  "two conflicts filled the record");

    row = scenarioHostConflict(scSlot, 0);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->kind == scnConflictRule,
                  "the first row is kind %d, wanted the rule", (int)row->kind);
    UT_ASSERT_MSG(strcmp(row->name, "tank_reload_ticks") == 0,
                  "the rule row names \"%s\"", row->name);
    UT_ASSERT_MSG(strcmp(row->winner, mapFile) == 0,
                  "the rule row says \"%s\" won, wanted the map's own "
                  "script, which is further up the list", row->winner);
    UT_ASSERT_MSG(strcmp(row->loser, "bothmod.lua") == 0,
                  "the rule row says \"%s\" lost, wanted \"bothmod.lua\"",
                  row->loser);

    row = scenarioHostConflict(scSlot, 1);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->kind == scnConflictRegion,
                  "the second row is kind %d, wanted the region",
                  (int)row->kind);
    UT_ASSERT_MSG(strcmp(row->name, "north") == 0,
                  "the region row names \"%s\"", row->name);
    UT_ASSERT_MSG(strcmp(row->winner, mapFile) == 0,
                  "the region row says \"%s\" won, wanted \"%s\"",
                  row->winner, mapFile);
    UT_ASSERT_MSG(strcmp(row->loser, "bothmod.lua") == 0,
                  "the region row says \"%s\" lost, wanted \"bothmod.lua\"",
                  row->loser);

    /* Nothing past the last row, so a reader that walks until the getter
       answers NULL reads the two and stops. */
    UT_ASSERT_MSG(scenarioHostConflict(scSlot, 2) == NULL,
                  "the record answered a row past its count");

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 18. The map's own script goes where the host's list puts it ───── */

/* Two composes of one pair of scripts, the map's own scenario ahead of the
   mod in the first and behind it in the second. What the case is for is the
   two things that have to be true at once: the host's order is obeyed, and
   the bits the regions answer to do not move when it is.

   The second list is how a host says it. The map's own row is left on the
   list, at the place the host wants the script composed, and the command bus
   lets that one bound name through where it refuses every other. A list that
   does not name it composes it at the front, which is the older behaviour and
   the first pass here. */
int run_scenario_compose_map_script_placed(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const ScnDirEntry      *row;
    const char             *mod[1] = { "modone.lua" };
    const char             *both[2];
    char                    mapFile[SCN_DIR_FILE_LEN];
    int                     northFirst;
    int                     southFirst;

    UT_ASSERT(scMakeDir("map_placed"));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scPutMapScript(kScMapScenario));

    /* The first pass: the host names the mod and nothing else, so the map's
       own script composes at the front. */
    sim = scSim();
    UT_ASSERT(sim != NULL);
    scCommit(sim);
    UT_ASSERT_MSG(scPick(sim, mod, 1) == CMD_OK, "the mod was refused");
    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));
    UT_ASSERT(scenarioHostScriptCount(scSlot) == 2);
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(m->regions[0].name, "north") == 0,
                  "region 0 is \"%s\", wanted the map's own at the front",
                  m->regions[0].name);
    northFirst = scRegionBit(m, "north");
    southFirst = scRegionBit(m, "south");
    UT_ASSERT(northFirst >= 0 && southFirst >= 0);
    UT_ASSERT_MSG(northFirst != southFirst,
                  "both regions answer to bit %d, so one mask bit is two "
                  "rectangles", northFirst);

    /* The name the map's own row is published under, which is what a host
       puts on its list to say where the script goes. Copied out before the
       sim goes, because the row is the sim's. */
    row = serverSimGetMapScript(sim);
    UT_ASSERT_MSG(row != NULL, "the map's own script was not published");
    UT_ASSERT_MSG(row->bound, "the map's own row is not bound");
    snprintf(mapFile, sizeof(mapFile), "%s", row->file);
    scDestroy(sim);

    /* The second pass: the same two scripts, the mod first. */
    sim = scSim();
    UT_ASSERT(sim != NULL);
    scCommit(sim);
    both[0] = "modone.lua";
    both[1] = mapFile;
    UT_ASSERT_MSG(scPick(sim, both, 2) == CMD_OK,
                  "a list naming the committed map's own script was refused");
    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the mod and the "
                  "map's own", scenarioHostScriptCount(scSlot));

    /* The array is packed in list order, so the mod's region is the one at
       the front now. This is the half that moved. */
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->numRegions == 2,
                  "the composite holds %d regions, wanted two",
                  (int)m->numRegions);
    UT_ASSERT_MSG(strcmp(m->regions[0].name, "south") == 0,
                  "region 0 is \"%s\", wanted the mod's, which the host put "
                  "first", m->regions[0].name);
    UT_ASSERT_MSG(strcmp(m->regions[1].name, "north") == 0,
                  "region 1 is \"%s\", wanted the map's own",
                  m->regions[1].name);

    /* And this is the half that did not. Each region answers to the bit it
       answered to in the first pass, because the bit is worked out from the
       file that named the region and the region's own name. */
    UT_ASSERT_MSG(scRegionBit(m, "north") == northFirst,
                  "\"north\" moved from bit %d to bit %d when the list was "
                  "reordered", northFirst, scRegionBit(m, "north"));
    UT_ASSERT_MSG(scRegionBit(m, "south") == southFirst,
                  "\"south\" moved from bit %d to bit %d when the list was "
                  "reordered", southFirst, scRegionBit(m, "south"));

    /* The lobby is told the host's list and not a copy of the map's row in
       front of it: the row is already on the list, and a second one would
       draw the same script twice in the chooser. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "the lobby list is %d rows, wanted the host's two",
                  serverSimGetLobbyScriptCount(sim));
    UT_ASSERT(strcmp(serverSimGetLobbyScript(sim, 0)->file,
                     "modone.lua") == 0);
    UT_ASSERT_MSG(strcmp(serverSimGetLobbyScript(sim, 1)->file, mapFile) == 0,
                  "row 1 of the lobby list is \"%s\", wanted the map's own",
                  serverSimGetLobbyScript(sim, 1)->file);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── The scripts the per-script table cases compose ───────────────── */

/* A base that says nothing until the round is ticking, and then calls the
   three library functions the mod below clears in its own copy. A raise from
   any of them fails the hook and leaves the line unwritten, which the case
   reads as the mod's write having reached this script. */
static const char kScLibBase[] =
    "scenario = {\n"
    "  name = \"Library Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    "local said = false\n"
    "function on_tick()\n"
    "  if said then return end\n"
    "  said = true\n"
    "  local t = {}\n"
    "  table.insert(t, 5)\n"
    "  local at = string.find(\"abc\", \"b\")\n"
    "  print(\"note:base_find=\" .. tostring(at) .. \";\")\n"
    "  print(\"note:base_floor=\" .. tostring(math.floor(2.5)) .. \";\")\n"
    "  print(\"note:base_insert=\" .. tostring(#t) .. \";\")\n"
    "end\n";

static const char kScLibMod[] =
    "scenario = {\n"
    "  name = \"Library Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "string.find = nil\n"
    "math.floor = nil\n"
    "table.insert = nil\n"
    "print(\"note:mod_find_nil=\" .. tostring(string.find == nil) .. \";\")\n"
    "print(\"note:mod_floor_nil=\" .. tostring(math.floor == nil) .. \";\")\n"
    "print(\"note:mod_insert_nil=\" .. tostring(table.insert == nil)\n"
    "      .. \";\")\n";

/* A base that reads back, from on_setup, what the mods below change in their
   own game. Every mod's top level has run by then, so a write that reached
   this script would already be showing.

   spawn_bot is called with no argument: the host's row raises for the
   missing table and does nothing else, and the mod's wrapper would answer
   "wrapped". Which of the two answered is what tells them apart. */
static const char kScGameBase[] =
    "scenario = {\n"
    "  name = \"Game Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    "function on_setup()\n"
    "  print(\"note:base_end=\" .. type(game.end_round) .. \";\")\n"
    "  local ok, r = pcall(game.spawn_bot)\n"
    "  print(\"note:base_spawn=\" .. tostring(ok) .. \",\" .. tostring(r)\n"
    "        .. \";\")\n"
    "  print(\"note:base_road=\" .. tostring(game.TERRAIN.road) .. \";\")\n"
    "  print(\"note:base_row=\" .. type(game.my_row) .. \";\")\n"
    "end\n";

static const char kScGameMod[] =
    "scenario = {\n"
    "  name = \"Game Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "game.end_round = nil\n"
    "game.spawn_bot = function() return \"wrapped\" end\n"
    "print(\"note:mod_end=\" .. type(game.end_round) .. \";\")\n"
    "print(\"note:mod_spawn=\" .. tostring(game.spawn_bot()) .. \";\")\n";

static const char kScTerrainMod[] =
    "scenario = {\n"
    "  name = \"Terrain Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "game.TERRAIN.road = 999\n"
    "print(\"note:mod_road=\" .. tostring(game.TERRAIN.road) .. \";\")\n";

/* A row of the script's own on game, which is what the compat prelude in
   tests/scenario/ does for the rows it adds. */
static const char kScRowMod[] =
    "scenario = {\n"
    "  name = \"Row Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    "game.my_row = function() return 7 end\n"
    "print(\"note:mod_row=\" .. tostring(game.my_row()) .. \";\")\n";

/* Under -allow-unsafe-scripts the base reads the mod's row by calling it,
   which only answers where the two scripts share one game. */
static const char kScRowReaderBase[] =
    "scenario = {\n"
    "  name = \"Row Reader\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    "function on_setup()\n"
    "  local v = nil\n"
    "  if type(game.my_row) == \"function\" then v = game.my_row() end\n"
    "  print(\"note:base_row=\" .. tostring(v) .. \";\")\n"
    "end\n";

/* How many fields game holds, counted by each script at its top level. */
#define SC_COUNT_GAME(tag)                                                   \
    "local n = 0\n"                                                          \
    "for _ in pairs(game) do n = n + 1 end\n"                                \
    "print(\"note:" tag "_count=\" .. n .. \";\")\n"

static const char kScCountBase[] =
    "scenario = {\n"
    "  name = \"Count Base\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "  game = \"tournament\",\n"
    "}\n"
    SC_COUNT_GAME("base");

static const char kScCountMod[] =
    "scenario = {\n"
    "  name = \"Count Mod\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n"
    SC_COUNT_GAME("mod");

/* ── 19. Each script's libraries are its own ──────────────────────── */

int run_scenario_compose_library_copy_per_script(void) {
    ServerSim  *sim;
    const char *picks[1] = { "libmod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("library_copy"));
    UT_ASSERT(scWrite("libmod.lua", kScLibMod));
    UT_ASSERT(scPutMapScript(kScLibBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(scTickForTank(sim), "slot 0 never had a tank");
    scReadNote(said, sizeof(said));

    /* The mod cleared its own three names. */
    UT_ASSERT_MSG(strstr(said, "mod_find_nil=true;") != NULL &&
                      strstr(said, "mod_floor_nil=true;") != NULL &&
                      strstr(said, "mod_insert_nil=true;") != NULL,
                  "the mod did not see its own writes: %s", said);

    /* And the base still has all three. */
    UT_ASSERT_MSG(strstr(said, "base_find=2;") != NULL,
                  "the base lost string.find to the mod: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_floor=2;") != NULL,
                  "the base lost math.floor to the mod: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_insert=1;") != NULL,
                  "the base lost table.insert to the mod: %s", said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 20. Each script's game is its own ────────────────────────────── */

int run_scenario_compose_game_copy_per_script(void) {
    ServerSim  *sim;
    const char *picks[1] = { "gamemod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("game_copy"));
    UT_ASSERT(scWrite("gamemod.lua", kScGameMod));
    UT_ASSERT(scPutMapScript(kScGameBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    UT_ASSERT_MSG(strstr(said, "mod_end=nil;") != NULL,
                  "the mod did not see its own game.end_round cleared: %s",
                  said);
    UT_ASSERT_MSG(strstr(said, "mod_spawn=wrapped;") != NULL,
                  "the mod did not see its own game.spawn_bot: %s", said);

    UT_ASSERT_MSG(strstr(said, "base_end=function;") != NULL,
                  "the base lost game.end_round to the mod: %s", said);
    /* The host's row raised for the missing table; the mod's wrapper would
       have answered. */
    UT_ASSERT_MSG(strstr(said, "base_spawn=false,") != NULL,
                  "the base's game.spawn_bot is not the host's row: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_spawn=true,wrapped;") == NULL,
                  "the base called the mod's game.spawn_bot: %s", said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 21. The tables inside game are each script's own ─────────────── */

int run_scenario_compose_game_nested_copy(void) {
    ServerSim  *sim;
    const char *picks[1] = { "terrainmod.lua" };
    char        said[8192];
    char        want[64];

    UT_ASSERT(scMakeDir("game_nested"));
    UT_ASSERT(scWrite("terrainmod.lua", kScTerrainMod));
    UT_ASSERT(scPutMapScript(kScGameBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    UT_ASSERT_MSG(strstr(said, "mod_road=999;") != NULL,
                  "the mod did not see its own game.TERRAIN.road: %s", said);
    snprintf(want, sizeof(want), "base_road=%d;", (int)ROAD);
    UT_ASSERT_MSG(strstr(said, want) != NULL,
                  "the base's game.TERRAIN.road is not %d: %s", (int)ROAD,
                  said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 22. A row a script adds to game stays its own ────────────────── */

int run_scenario_compose_compat_write_stays_local(void) {
    ServerSim  *sim;
    const char *picks[1] = { "rowmod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("compat_local"));
    UT_ASSERT(scWrite("rowmod.lua", kScRowMod));
    UT_ASSERT(scPutMapScript(kScGameBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    UT_ASSERT_MSG(strstr(said, "mod_row=7;") != NULL,
                  "the mod could not call the row it added: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_row=nil;") != NULL,
                  "the mod's row reached the base: %s", said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 23. pairs(game) sees every field the host installed ──────────── */

int run_scenario_compose_pairs_game_complete(void) {
    ServerSim  *sim;
    const char *picks[1] = { "countmod.lua" };
    char        said[8192];
    char        want[64];
    size_t      rows   = 0;
    size_t      consts = 0;
    size_t      words  = 0;
    size_t      fields;

    /* What scenarioLuaInstall puts on game: a closure per row, a number per
       constant, TERRAIN, and one table per word table. */
    (void)scenarioLuaRows(&rows);
    (void)scenarioLuaConsts(&consts);
    (void)scenarioLuaWordTables(&words);
    fields = rows + consts + 1 + words;

    UT_ASSERT(scMakeDir("pairs_game"));
    UT_ASSERT(scWrite("countmod.lua", kScCountMod));
    UT_ASSERT(scPutMapScript(kScCountBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    snprintf(want, sizeof(want), "base_count=%d;", (int)fields);
    UT_ASSERT_MSG(strstr(said, want) != NULL,
                  "the base's game does not hold the %d fields installed: %s",
                  (int)fields, said);
    snprintf(want, sizeof(want), "mod_count=%d;", (int)fields);
    UT_ASSERT_MSG(strstr(said, want) != NULL,
                  "the mod's game does not hold the %d fields installed: %s",
                  (int)fields, said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── 24. Under -allow-unsafe-scripts game is shared ───────────────── */

/* The switch is one answer for the process, so it is set around the body
   and cleared whichever way the body returned: an assertion's early return
   lands back here rather than skipping the reset, as it does for the unsafe
   cases in test_scenario_sandbox.c. */
static int scUnsafeRun(int (*body)(void)) {
    int rc;

    scenarioHostSetUnsafeScripts(true);
    rc = body();
    scenarioHostSetUnsafeScripts(false);
    return rc;
}

static int scUnsafeKeepsSharing(void) {
    ServerSim  *sim;
    const char *picks[1] = { "rowmod.lua" };
    char        said[8192];

    UT_ASSERT(scMakeDir("unsafe_sharing"));
    UT_ASSERT(scWrite("rowmod.lua", kScRowMod));
    UT_ASSERT(scPutMapScript(kScRowReaderBase));
    sim = scRunningSim(picks, 1);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the base and the mod",
                  scenarioHostScriptCount(scSlot));
    scReadNote(said, sizeof(said));

    UT_ASSERT_MSG(strstr(said, "mod_row=7;") != NULL,
                  "the mod could not call the row it added: %s", said);
    UT_ASSERT_MSG(strstr(said, "base_row=7;") != NULL,
                  "the base did not see the mod's row on a shared game: %s",
                  said);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

int run_scenario_compose_unsafe_keeps_sharing(void) {
    return scUnsafeRun(scUnsafeKeepsSharing);
}

/* ── The Mods/Scenario setting ────────────────────────────────────── */

/* The lobby's Mods/Scenario checkbox, through the command it sends. off is
   the wire's sense: true keeps the host's picks out of the round. The
   dispatch arm asks for the decision again when the setting takes, so the
   compose these cases read afterwards is the one the box asked for. */
static CmdResult scModsOff(ServerSim *sim, bool off) {
    ClientCommand cmd;
    CmdResult     r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_SETTING;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetting.settingType = LST_MODS_OFF;
    cmd.u.lobbySetting.valueLen    = 1;
    cmd.u.lobbySetting.value[0]    = off ? 1 : 0;
    /* The setting recomposes the round, which a pick made just before it
       would otherwise stop on its one-list-a-second cooldown. */
    sim->scenarioPickTick = 0;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Box off on a map that brings its own script, with a picked scenario and a
   mod on the list: the map's own script plays alone. The picks stay on the
   list, and the lobby is told the map's row at the front with them behind
   it, which is the list a chooser then drafts from. */
int run_scenario_compose_off_map_script_only(void) {
    ServerSim         *sim;
    const ScnDirEntry *row;
    const char        *picks[2] = { "duel.lua", "modone.lua" };

    UT_ASSERT(scMakeDir("off_map_only"));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT_MSG(scPick(sim, picks, 2) == CMD_OK,
                  "the scenario and the mod were refused");
    UT_ASSERT_MSG(scModsOff(sim, true) == CMD_OK,
                  "switching Mods/Scenario off was refused");

    UT_ASSERT_MSG(scSlot != NULL, "nothing attached with the box off: %s",
                  scenarioHostLastError(scSlot));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 1,
                  "the round composed %d scripts with the box off, wanted "
                  "the map's own alone",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioMap,
                  "the box off left the source at %d, wanted map (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Survival") == 0,
                  "the box off played \"%s\", wanted the map's own",
                  sim->scenarioIdentity.name);

    /* The host's list is left as it was written. */
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 2,
                  "the host's list holds %d with the box off, wanted its two",
                  serverSimGetScriptCount(sim));
    /* And the lobby is told three rows: the map's own first and bound, the
       two picks behind it. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 3,
                  "the lobby was published %d rows, wanted three",
                  serverSimGetLobbyScriptCount(sim));
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->bound && strcmp(row->name, "Survival") == 0,
                  "row 0 is \"%s\" (bound %d), wanted the map's own",
                  row->name, (int)row->bound);
    row = serverSimGetLobbyScript(sim, 1);
    UT_ASSERT(row != NULL && !row->bound);
    UT_ASSERT_MSG(strcmp(row->file, "duel.lua") == 0,
                  "row 1 is \"%s\", wanted the picked scenario", row->file);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* Box off on a plain map: the picks are all there is, and none of them
   composes, so nothing plays. */
int run_scenario_compose_off_plain_map_none(void) {
    ServerSim  *sim;
    const char *picks[2] = { "duel.lua", "modone.lua" };

    UT_ASSERT(scMakeDir("off_plain_none"));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    /* No script beside the map, which is what makes it a plain map. */
    scDropMapScript();
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT_MSG(scPick(sim, picks, 2) == CMD_OK,
                  "the scenario and the mod were refused");
    UT_ASSERT_MSG(scSlot != NULL && scenarioHostScriptCount(scSlot) == 2,
                  "with the box on the plain map did not compose both picks");

    UT_ASSERT_MSG(scModsOff(sim, true) == CMD_OK,
                  "switching Mods/Scenario off was refused");
    UT_ASSERT_MSG(scSlot == NULL,
                  "the box off on a plain map still attached %d scripts",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioNone,
                  "the box off on a plain map left the source at %d, wanted "
                  "none (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioNone);
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "a plain map published a row for a script it has not got");

    scDestroy(sim);
    scDropDir();
    return 0;
}

/* Box on, a map with its own script, and a picked scenario with a mod: the
   picks play, and the picked scenario replaces the map's own, which is no
   longer on the list the lobby is told. */
int run_scenario_compose_on_picks_replace_map(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const ScnDirEntry      *row;
    const char             *picks[2] = { "duel.lua", "modone.lua" };

    UT_ASSERT(scMakeDir("on_replace_map"));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT_MSG(!serverSimGetModsOff(sim), "a fresh sim had the box off");
    UT_ASSERT_MSG(scPick(sim, picks, 2) == CMD_OK,
                  "the scenario and the mod were refused");

    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the two picks",
                  scenarioHostScriptCount(scSlot));
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(strcmp(m->name, "Duel") == 0,
                  "the composite is named \"%s\", wanted the picked scenario",
                  m->name);
    UT_ASSERT_MSG(!scHasRegion(m, "north"),
                  "the map's own region is in a round it does not play");
    UT_ASSERT_MSG(scHasRegion(m, "south"), "the mod's region is missing");
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "the map's row outlived the scenario that replaced it");
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "the lobby was published %d rows, wanted the two picks",
                  serverSimGetLobbyScriptCount(sim));
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL && !row->bound);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* Box off and then on again, with the list a chooser sends from the off
   state in between.

   With the box off the lobby shows the map's own row in front of the picked
   scenario. A chooser that sent every row it drafted from that list named
   two scenarios, which CMD_SET_SCRIPT_LIST refuses — and the map's row
   cannot be dropped in the chooser, so the host could not change the list
   at all. The chooser now leaves the map's row off beside a picked scenario
   (lobbyRoundBoundShadowed); the list without it is taken, and it reads as
   the picked scenario replacing the map's own when the box goes back on. */
int run_scenario_compose_off_then_on(void) {
    ServerSim  *sim;
    char        mapFile[SERVER_SCENARIO_FILE_LEN];
    const char *picks[2]     = { "duel.lua", "modone.lua" };
    const char *reordered[2] = { "modone.lua", "duel.lua" };
    const char *withMap[3];

    UT_ASSERT(scMakeDir("off_then_on"));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    UT_ASSERT(scWrite("modone.lua", kScModOne));
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT(serverSimGetMapScript(sim) != NULL);
    SDL_strlcpy(mapFile, serverSimGetMapScript(sim)->file, sizeof(mapFile));
    UT_ASSERT(scPick(sim, picks, 2) == CMD_OK);
    UT_ASSERT_MSG(scModsOff(sim, true) == CMD_OK,
                  "switching Mods/Scenario off was refused");
    UT_ASSERT(scSlot != NULL && scenarioHostScriptCount(scSlot) == 1);

    /* The list as the lobby shows it, map's row and all, is still two
       scenarios, and still refused. This is the list the chooser no longer
       sends. */
    withMap[0] = mapFile;
    withMap[1] = "modone.lua";
    withMap[2] = "duel.lua";
    UT_ASSERT_MSG(scPick(sim, withMap, 3) == CMD_REJECT_INVALID,
                  "a list naming the map's own scenario and a picked one "
                  "was taken");

    /* The list the chooser sends instead: the host's edit, the map's row
       left off. Taken, and with the box off the map's own still plays. */
    UT_ASSERT_MSG(scPick(sim, reordered, 2) == CMD_OK,
                  "the chooser's list without the map's row was refused");
    UT_ASSERT_MSG(scSlot != NULL && scenarioHostScriptCount(scSlot) == 1,
                  "with the box off the edited list composed %d scripts, "
                  "wanted the map's own alone",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(sim->scenarioIdentity.source == lobbyScenarioMap,
                  "with the box off the source read %d, wanted map (%d)",
                  (int)sim->scenarioIdentity.source, (int)lobbyScenarioMap);
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 3,
                  "the lobby was published %d rows, wanted the map's own and "
                  "both picks",
                  serverSimGetLobbyScriptCount(sim));

    /* And back on: not refused, and the picks play in the edited order, the
       picked scenario in place of the map's own. */
    UT_ASSERT_MSG(scModsOff(sim, false) == CMD_OK,
                  "switching Mods/Scenario back on was refused");
    UT_ASSERT_MSG(scSlot != NULL && scenarioHostScriptCount(scSlot) == 2,
                  "with the box back on %d scripts composed, wanted the two "
                  "picks",
                  scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.name, "Duel") == 0,
                  "with the box back on the round is \"%s\", wanted the "
                  "picked scenario",
                  sim->scenarioIdentity.name);
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "the map's row outlived the scenario that replaced it");

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* Box off, a map that brings its own script, and a full list of ten picks.

   The lobby list is held at LOBBY_SCRIPT_LIST_MAX, and with the box off the
   map's own row goes in front of the picks, so the last pick is not
   published (serverSimGetLobbyScriptCount). A chooser drafts from what was
   published, and a list sent from that draft names nine picks: the tenth
   leaves the round although the host never took it out. Nine picks publish
   exactly the same shape, so a chooser cannot tell the two apart, and it
   refuses every edit while the list it was told is full with the map's row
   in it (lobbyRoundLiveMayBeCut in lobby_scenario_chooser.cpp). With the box
   on the map's row goes and all ten are shown. */
int run_scenario_compose_off_full_list_hides_last(void) {
    ServerSim         *sim;
    const ScnDirEntry *row;
    char               names[9][16];
    const char        *picks[10];
    const char        *shown[9];
    int                i;

    UT_ASSERT(scMakeDir("off_full_hides_last"));
    UT_ASSERT(scWrite("duel.lua", kScOtherScenario));
    picks[0] = "duel.lua";
    for (i = 0; i < 9; i++) {
        snprintf(names[i], sizeof(names[i]), "mod%d.lua", i + 1);
        UT_ASSERT(scWrite(names[i], kScModPlain));
        picks[i + 1] = names[i];
    }
    UT_ASSERT(scPutMapScript(kScMapScenario));
    sim = scSim();
    UT_ASSERT(sim != NULL);

    scCommit(sim);
    UT_ASSERT_MSG(scModsOff(sim, true) == CMD_OK,
                  "switching Mods/Scenario off was refused");
    UT_ASSERT_MSG(scPick(sim, picks, 10) == CMD_OK,
                  "a list of ten picks was refused");
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 10,
                  "the host's list holds %d, wanted all ten",
                  serverSimGetScriptCount(sim));

    /* The map's row and nine picks: the tenth is not published. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == LOBBY_SCRIPT_LIST_MAX,
                  "the lobby was published %d rows, wanted the cap of %d",
                  serverSimGetLobbyScriptCount(sim), LOBBY_SCRIPT_LIST_MAX);
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(row->bound, "row 0 is \"%s\", wanted the map's own",
                  row->file);
    row = serverSimGetLobbyScript(sim, LOBBY_SCRIPT_LIST_MAX - 1);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->file, "mod8.lua") == 0,
                  "the last published row is \"%s\", wanted the ninth pick",
                  row->file);

    /* The list a chooser would send from those rows, the map's row left off
       as it always is beside a picked scenario. The tenth pick goes. */
    for (i = 0; i < 9; i++) {
        shown[i] = serverSimGetLobbyScript(sim, i + 1)->file;
    }
    UT_ASSERT(scPick(sim, shown, 9) == CMD_OK);
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 9,
                  "the host's list holds %d after a send of what was shown, "
                  "wanted nine",
                  serverSimGetScriptCount(sim));

    /* And nine picks publish the same shape as ten did, which is what keeps
       a chooser from knowing whether a pick is hidden. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == LOBBY_SCRIPT_LIST_MAX,
                  "nine picks published %d rows, wanted the cap of %d",
                  serverSimGetLobbyScriptCount(sim), LOBBY_SCRIPT_LIST_MAX);
    row = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(row != NULL && row->bound);

    /* Box on, with all ten picked again: the map's row goes and every pick
       is published. */
    UT_ASSERT(scPick(sim, picks, 10) == CMD_OK);
    UT_ASSERT_MSG(scModsOff(sim, false) == CMD_OK,
                  "switching Mods/Scenario back on was refused");
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "the map's row outlived the scenario that replaced it");
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 10,
                  "with the box on the lobby was published %d rows, wanted "
                  "all ten picks",
                  serverSimGetLobbyScriptCount(sim));
    row = serverSimGetLobbyScript(sim, 9);
    UT_ASSERT(row != NULL);
    UT_ASSERT_MSG(strcmp(row->file, "mod9.lua") == 0 && !row->bound,
                  "with the box on the last row is \"%s\", wanted the tenth "
                  "pick",
                  row->file);

    scDestroy(sim);
    scDropMapScript();
    scDropDir();
    return 0;
}

/* ── needs_bots: any script on the list ──────────────────────────── */

/* Put the lobby on "no computer tanks", the setting a needs_bots list moves
   it off. */
static void scNoBots(ServerSim *sim) {
    serverSimSetAiPolicy(sim, (uint8_t)aiNone);
    serverSimSetBotAiType(sim, aiNone);
}

/* A mod without needs_bots leaves the lobby on aiNone. A second mod that says
   it moves the lobby off aiNone, because the list needs bots when any script
   on it does. Taking every script off gives aiNone back. */
int run_scenario_compose_needs_bots_any_mod(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const char             *plain[1] = { "plain.lua" };
    const char             *both[2]  = { "plain.lua", "horde.lua" };

    UT_ASSERT(scMakeDir("needs_bots_any_mod"));
    UT_ASSERT(scWrite("plain.lua", kScModPlain));
    UT_ASSERT(scWrite("horde.lua", kScModNeedsBots));
    scDropMapScript();
    sim = scSim();
    UT_ASSERT(sim != NULL);
    scNoBots(sim);
    scCommit(sim);

    UT_ASSERT_MSG(scPick(sim, plain, 1) == CMD_OK, "the plain mod was refused");
    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(!m->needsBots,
                  "a list of one mod without needs_bots composed as needing "
                  "bots");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiNone,
                  "a mod without needs_bots moved the AI policy to %d",
                  (int)serverSimGetBotAiType(sim));

    UT_ASSERT_MSG(scPick(sim, both, 2) == CMD_OK, "the two mods were refused");
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->needsBots,
                  "a list with one needs_bots mod composed as not needing "
                  "bots");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiYes,
                  "a list with a needs_bots mod left the AI policy at %d, "
                  "wanted aiYes (%d)", (int)serverSimGetBotAiType(sim),
                  (int)aiYes);

    UT_ASSERT_MSG(scPick(sim, NULL, 0) == CMD_OK,
                  "clearing the list was refused");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiNone,
                  "the last script going left the AI policy at %d, wanted the "
                  "aiNone (%d) the lobby was on",
                  (int)serverSimGetBotAiType(sim), (int)aiNone);

    scDestroy(sim);
    scDropDir();
    return 0;
}

/* A map's own scenario that says needs_bots, with a mod behind it that does
   not, moves the lobby off aiNone; a plain map committed after it gives
   aiNone back. */
int run_scenario_compose_needs_bots_scenario_and_mod(void) {
    ServerSim              *sim;
    const ScenarioManifest *m;
    const char             *picks[1] = { "plain.lua" };

    UT_ASSERT(scMakeDir("needs_bots_scenario_and_mod"));
    UT_ASSERT(scWrite("plain.lua", kScModPlain));
    UT_ASSERT(scPutMapScript(kScScenarioNeedsBots));
    sim = scSim();
    UT_ASSERT(sim != NULL);
    scNoBots(sim);

    scCommit(sim);
    UT_ASSERT_MSG(scPick(sim, picks, 1) == CMD_OK, "the mod was refused");
    UT_ASSERT_MSG(scSlot != NULL, "nothing attached: %s",
                  scenarioHostLastError(scSlot));
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 2,
                  "the round composed %d scripts, wanted the map's and the "
                  "mod", scenarioHostScriptCount(scSlot));
    m = scenarioHostManifest(scSlot);
    UT_ASSERT(m != NULL);
    UT_ASSERT_MSG(m->needsBots,
                  "a needs_bots scenario with a plain mod composed as not "
                  "needing bots");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiYes,
                  "a needs_bots scenario and a mod left the AI policy at %d, "
                  "wanted aiYes (%d)", (int)serverSimGetBotAiType(sim),
                  (int)aiYes);

    /* The map's script goes with the map, and the pick goes too. */
    scDropMapScript();
    UT_ASSERT(scPick(sim, NULL, 0) == CMD_OK);
    scCommit(sim);
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiNone,
                  "a plain map after the needs_bots scenario left the AI "
                  "policy at %d, wanted aiNone (%d)",
                  (int)serverSimGetBotAiType(sim), (int)aiNone);

    scDestroy(sim);
    scDropDir();
    return 0;
}

/* ── on_tick runs up the list ─────────────────────────────────────── */

/* Two mods that each, for their first two frames with a tank, say the speed
   the tank has and then write their own. game.set_modifiers replaces the
   whole set, so the last write in a frame is the one that stands. on_tick
   runs up the list, the bottom script first, so the top script writes last
   and its speed is the one the tank keeps — the same script "the top of the
   list wins" gives a rule to. */
#define SC_ORDER_MOD(NAME, TAG, SPEED)                                       \
    "scenario = {\n"                                                         \
    "  name = \"" NAME "\",\n"                                               \
    "  api = 1,\n"                                                           \
    "  kind = \"mod\",\n"                                                    \
    "  bound = false,\n"                                                     \
    "}\n"                                                                    \
    "local n = 0\n"                                                          \
    "function on_tick(tick)\n"                                               \
    "  local t = game.tank(0)\n"                                             \
    "  if t == nil or n >= 2 then return end\n"                              \
    "  n = n + 1\n"                                                          \
    "  print(\"note:" TAG "\" .. n .. \"=\" .. t.mods.speed .. \";\")\n"    \
    "  game.set_modifiers(0, { speed = " SPEED " })\n"                       \
    "end\n"

static const char kScOrderFast[] = SC_ORDER_MOD("Order Fast", "fast", "300");
static const char kScOrderSlow[] = SC_ORDER_MOD("Order Slow", "slow", "150");

int run_scenario_compose_on_tick_bottom_first(void) {
    ServerSim  *sim;
    const char *fastOnTop[2] = { "orderfast.lua", "orderslow.lua" };
    const char *slowOnTop[2] = { "orderslow.lua", "orderfast.lua" };
    char        said[8192];
    TankInfo    info;
    int         i;

    UT_ASSERT(scMakeDir("on_tick_bottom_first"));
    UT_ASSERT(scWrite("orderfast.lua", kScOrderFast));
    UT_ASSERT(scWrite("orderslow.lua", kScOrderSlow));
    UT_ASSERT(scPutMapScript(kScQuietBase));

    /* Fast on top. Slow, at the bottom, runs first in each frame: in the
       second frame it sees what fast wrote last in the first, and fast sees
       what slow wrote a moment before in the same frame. */
    sim = scRunningSim(fastOnTop, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scenarioHostScriptCount(scSlot) == 3,
                  "the round composed %d scripts, wanted the base and two "
                  "mods", scenarioHostScriptCount(scSlot));
    UT_ASSERT_MSG(scTickForTank(sim), "slot 0 never got a tank");
    for (i = 0; i < 4; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT(serverSimGetTankInfo(sim, 0, &info) && info.has_tank);
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "slow1=") != NULL &&
                  strstr(said, "fast1=") != NULL &&
                  strstr(said, "slow1=") < strstr(said, "fast1="),
                  "the bottom script's on_tick did not run first: %s", said);
    UT_ASSERT_MSG(strstr(said, "fast1=150;") != NULL,
                  "the top script did not see the bottom script's write from "
                  "the same frame: %s", said);
    UT_ASSERT_MSG(strstr(said, "slow2=300;") != NULL,
                  "the frame did not end on the top script's speed: %s", said);
    UT_ASSERT_MSG(strstr(said, "fast2=150;") != NULL,
                  "the second frame did not run bottom first: %s", said);
    scDestroy(sim);

    /* The same two files the other way round give the other answer, so the
       order read above is the list's and not the files'. */
    sim = scRunningSim(slowOnTop, 2);
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(scTickForTank(sim), "slot 0 never got a tank");
    for (i = 0; i < 4; i++) {
        serverSimTick(sim);
    }
    scReadNote(said, sizeof(said));
    UT_ASSERT_MSG(strstr(said, "fast1=") != NULL &&
                  strstr(said, "slow1=") != NULL &&
                  strstr(said, "fast1=") < strstr(said, "slow1="),
                  "with the list flipped the bottom script's on_tick did not "
                  "run first: %s", said);
    UT_ASSERT_MSG(strstr(said, "slow1=300;") != NULL &&
                  strstr(said, "fast2=150;") != NULL,
                  "with the list flipped the frame did not end on the top "
                  "script's speed: %s", said);
    scDestroy(sim);

    scDropMapScript();
    scDropDir();
    return 0;
}

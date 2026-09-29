/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A lobby map change keeps the bots and the script list.
 *
 * Every step goes through the command the lobby sends — Add Bot, the bot's
 * config, Remove Bot, the script list and Set Map — so the map change under
 * test is the real one: serverSimReloadMap, the start reconcile, and the
 * scenario decision that follows it.
 *
 *   lobby_map_keeps_bots_plain — no script at all. Three bots with their own
 *       names, teams and difficulties survive a change to a map with fewer
 *       starts than players and a change back, and every seat still holds a
 *       start or none, never one past the map's list.
 *   lobby_map_keeps_bots_scenario — a picked scenario with a lobby of its
 *       own and a mod behind it. The host trims the scenario's seats, adds
 *       two bots and changes one's difficulty; two map changes later the
 *       roster is the same seat for seat, and the list is the same two
 *       scripts in the same order.
 *   lobby_map_keeps_bots_map_own_row — a map that brings its own script,
 *       with a mod picked over it. A plain map takes the map's row off the
 *       list and leaves the mod; the scripted map puts its row back. The
 *       bots stay through both.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* the lobby's commands */
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers, botConfigs, mapFilePath */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"   /* the script list and the map's own row */
#include "players.h"               /* playersGetPlayerName */
#include "starts.h"                /* startsGetNumStarts */
#include "scenario_host.h"
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

/* ── The scratch directory ────────────────────────────────────────── */

/* Two maps copied out of data/maps: Everard with sixteen starts, and one
   with four, which a lobby of seven cannot fit into. Named here so no
   script shipped beside the originals comes with them. */
#define MK_BIG_MAP     "big.map"
#define MK_SMALL_MAP   "small.map"
#define MK_BIG_SRC     "Everard Island.map"
#define MK_SMALL_SRC   "Gothic Industrial.map"
#define MK_SMALL_STARTS 4

static char mkMaps[512];
static char mkScripts[512];
static char mkBrain[512];

static bool mkCopy(const char *from, const char *to) {
    FILE  *in, *out;
    char   buf[4096];
    size_t n;
    bool   ok = true;

    in = fopen(from, "rb");
    if (in == NULL) return false;
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

static bool mkPut(const char *dir, const char *leaf, const char *text) {
    char  path[768];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

/* The maps, a scenarios directory and a fixture brain, all in this test's
   own scratch directory, which the process removes at exit. */
static bool mkSetUp(void) {
    char from[768], to[768];

    if (!utScratchPath(mkMaps, sizeof(mkMaps), "maps")) return false;
    if (!utScratchPath(mkScripts, sizeof(mkScripts), "scenarios")) {
        return false;
    }
    if (!utScratchPath(mkBrain, sizeof(mkBrain), "brain.lua")) return false;
    SDL_CreateDirectory(mkMaps);
    SDL_CreateDirectory(mkScripts);

    snprintf(from, sizeof(from), "%s/%s", WB_DATA_MAPS_DIR, MK_BIG_SRC);
    snprintf(to, sizeof(to), "%s/%s", mkMaps, MK_BIG_MAP);
    if (!mkCopy(from, to)) return false;
    snprintf(from, sizeof(from), "%s/%s", WB_DATA_MAPS_DIR, MK_SMALL_SRC);
    snprintf(to, sizeof(to), "%s/%s", mkMaps, MK_SMALL_MAP);
    if (!mkCopy(from, to)) return false;

    {
        FILE *f = fopen(mkBrain, "wb");
        if (f == NULL) return false;
        fputs("-- fixture\n", f);
        fclose(f);
    }
    return true;
}

/* A scenario with a lobby of its own: four held seats on team 3. */
static const char kMkWaves[] =
    "scenario = {\n"
    "  name = \"Waves\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  lobby = {\n"
    "    teams = { { id = 3, bots = 4, max_bots = 6, fielded = false } },\n"
    "  },\n"
    "}\n";

/* A mod, which plays behind whatever decides the round. */
static const char kMkMod[] =
    "scenario = {\n"
    "  name = \"Fast Reload\",\n"
    "  api = 1,\n"
    "  kind = \"mod\",\n"
    "  bound = false,\n"
    "}\n";

/* A map's own script, with no lobby of its own. */
static const char kMkMapOwn[] =
    "scenario = {\n"
    "  name = \"Big Island Rules\",\n"
    "  api = 1,\n"
    "  bound = true,\n"
    "}\n";

/* ── The sim ──────────────────────────────────────────────────────── */

static ScenarioHost *mkSlot;

/* The host in slot 0, not ready, so no roster change starts the round. The
   scratch maps directory is the map root the Set Map command resolves
   against, and the scratch scenarios directory is where a pick is looked
   up. */
static ServerSim *mkSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    sim->lobbyPlayers[0].ready = false;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, mkBrain);
    serverSimSetState(sim, serverStateLobby);
    (void)serverSimMapDirBuild(sim, mkMaps);

    mkSlot = NULL;
    serverSimSetScenarioDir(sim, mkScripts);
    scenarioHostRegisterScenarioLister(sim);
    scenarioHostFollowMap(sim, &mkSlot);
    return sim;
}

static void mkDestroy(ServerSim *sim) {
    /* The host holds registrations on the sim, so it goes first. */
    scenarioHostDetach(mkSlot);
    mkSlot = NULL;
    serverSimDestroy(sim);
}

static CmdResult mkApply(ServerSim *sim, const ClientCommand *cmd) {
    CmdResult r;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult mkSetMap(ServerSim *sim, const char *leaf) {
    ClientCommand cmd;
    size_t        n = strlen(leaf);

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_SET_MAP;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetMap.relPathLen = (uint8_t)n;
    memcpy(cmd.u.lobbySetMap.relPath, leaf, n);
    return mkApply(sim, &cmd);
}

/* Add Bot with a name and a team, answering the slot it landed in or -1. */
static int mkAddBot(ServerSim *sim, const char *name, BYTE team) {
    ClientCommand cmd;
    bool          before[MAX_TANKS];
    int           i;
    size_t        n = strlen(name);

    for (i = 0; i < MAX_TANKS; i++) {
        before[i] = sim->playerConnected[i] ? true : false;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_ADD_BOT;
    cmd.cmdSeq = 1;
    cmd.u.lobbyAddBot.teamNumber = team;
    cmd.u.lobbyAddBot.nameLen    = (uint8_t)n;
    memcpy(cmd.u.lobbyAddBot.name, name, n);
    if (mkApply(sim, &cmd) != CMD_OK) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!before[i] && sim->playerConnected[i]) return i;
    }
    return -1;
}

static CmdResult mkSetDifficulty(ServerSim *sim, BYTE slot, uint8_t level) {
    ClientCommand cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_BOT_CONFIG;
    cmd.cmdSeq = 1;
    cmd.u.lobbyBotConfig.slot        = slot;
    cmd.u.lobbyBotConfig.mode        = sim->botConfigs[slot].mode;
    cmd.u.lobbyBotConfig.difficulty  = level;
    cmd.u.lobbyBotConfig.personality = sim->botConfigs[slot].personality;
    return mkApply(sim, &cmd);
}

static CmdResult mkRemoveBot(ServerSim *sim, BYTE slot) {
    ClientCommand cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_REMOVE_BOT;
    cmd.cmdSeq = 1;
    cmd.u.lobbyRemoveBot.slot = slot;
    return mkApply(sim, &cmd);
}

static CmdResult mkPick(ServerSim *sim, const char *const *files, int count) {
    ClientCommand cmd;
    int           i;

    /* The pick gap is a second of ticks, and these cases do not tick. */
    sim->scenarioPickTick = 0;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 1;
    cmd.u.setScriptList.count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        snprintf(cmd.u.setScriptList.files[i],
                 sizeof(cmd.u.setScriptList.files[i]), "%s", files[i]);
    }
    return mkApply(sim, &cmd);
}

/* ── The roster, seat by seat ─────────────────────────────────────── */

typedef struct {
    bool    connected;
    bool    bot;
    bool    keepSeat;
    BYTE    team;
    uint8_t mode;
    uint8_t difficulty;
    char    name[PLAYER_NAME_LEN];
} MkSeat;

static void mkSnapshot(ServerSim *sim, MkSeat *out) {
    BYTE i;

    memset(out, 0, sizeof(MkSeat) * MAX_TANKS);
    for (i = 0; i < MAX_TANKS; i++) {
        out[i].connected = sim->playerConnected[i] ? true : false;
        if (!out[i].connected) continue;
        out[i].bot        = serverSimIsBot(sim, i);
        out[i].keepSeat   = sim->lobbyPlayers[i].keepSeat;
        out[i].team       = sim->lobbyPlayers[i].teamNumber;
        out[i].mode       = sim->botConfigs[i].mode;
        out[i].difficulty = sim->botConfigs[i].difficulty;
        playersGetPlayerName(&sim->sim.plyrs, i, out[i].name,
                             sizeof(out[i].name), TRUE);
    }
}

/* Every seat the same as it was: who is in it, bot or not, the scenario's or
   the host's, and the team, the mode, the difficulty and the name. */
static int mkSameRoster(const MkSeat *was, const MkSeat *now,
                        const char *when) {
    int i;

    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(was[i].connected == now[i].connected,
                      "%s: seat %d went from %s to %s", when, i,
                      was[i].connected ? "taken" : "free",
                      now[i].connected ? "taken" : "free");
        if (!was[i].connected) continue;
        UT_ASSERT_MSG(was[i].bot == now[i].bot,
                      "%s: seat %d changed between bot and human", when, i);
        UT_ASSERT_MSG(was[i].keepSeat == now[i].keepSeat,
                      "%s: seat %d changed hands between the scenario and "
                      "the host", when, i);
        UT_ASSERT_MSG(was[i].team == now[i].team,
                      "%s: seat %d moved from team %d to team %d", when, i,
                      (int)was[i].team, (int)now[i].team);
        UT_ASSERT_MSG(was[i].mode == now[i].mode,
                      "%s: seat %d mode went from %d to %d", when, i,
                      (int)was[i].mode, (int)now[i].mode);
        UT_ASSERT_MSG(was[i].difficulty == now[i].difficulty,
                      "%s: seat %d difficulty went from %d to %d", when, i,
                      (int)was[i].difficulty, (int)now[i].difficulty);
        UT_ASSERT_MSG(strcmp(was[i].name, now[i].name) == 0,
                      "%s: seat %d was renamed from \"%s\" to \"%s\"", when,
                      i, was[i].name, now[i].name);
    }
    return 0;
}

/* Every seat holds a start on the map now committed, or none, and no two
   seats hold the same one. Answers how many hold one. */
static int mkStartsValid(ServerSim *sim, int *held, const char *when) {
    BYTE numStarts = startsGetNumStarts(&sim->sim.ss);
    bool taken[256];
    int  i;

    memset(taken, 0, sizeof(taken));
    *held = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        BYTE s;
        if (!sim->playerConnected[i]) continue;
        s = sim->lobbyPlayers[i].startIdx;
        if (s == 0xFF) continue;
        UT_ASSERT_MSG(s >= 1 && s <= numStarts,
                      "%s: seat %d holds start %d of a map with %d", when, i,
                      (int)s, (int)numStarts);
        UT_ASSERT_MSG(!taken[s], "%s: start %d is held twice", when, (int)s);
        taken[s] = true;
        (*held)++;
    }
    return 0;
}

static int mkBots(const ServerSim *sim) {
    int n = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot((ServerSim *)sim, i)) n++;
    }
    return n;
}

static int mkScriptListIs(ServerSim *sim, const char *const *files,
                          int count, const char *when) {
    int i;

    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == count,
                  "%s: the host's list holds %d scripts, expected %d", when,
                  serverSimGetScriptCount(sim), count);
    for (i = 0; i < count; i++) {
        const ScnDirEntry *row = serverSimGetScript(sim, i);
        UT_ASSERT_MSG(row != NULL && strcmp(row->file, files[i]) == 0,
                      "%s: entry %d of the list is \"%s\", expected \"%s\"",
                      when, i, row != NULL ? row->file : "(none)", files[i]);
    }
    return 0;
}

/* ── No script at all ─────────────────────────────────────────────── */

int run_lobby_map_keeps_bots_plain(void) {
    static const char *const kNames[] = {
        "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"
    };
    static const BYTE kTeams[] = { 1, 2, 2, 1, 1, 2 };
    ServerSim *sim;
    MkSeat     was[MAX_TANKS], now[MAX_TANKS];
    int        slots[6];
    int        i, held;

    UT_ASSERT(mkSetUp());
    ut_brain_stub_arm(true);
    sim = mkSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(mkSetMap(sim, MK_BIG_MAP) == CMD_OK,
                  "the big map was refused");

    for (i = 0; i < 6; i++) {
        slots[i] = mkAddBot(sim, kNames[i], kTeams[i]);
        UT_ASSERT_MSG(slots[i] > 0, "Add Bot for %s was refused", kNames[i]);
    }
    /* Three difficulties the host picked by hand, none of them the
       default. */
    UT_ASSERT(mkSetDifficulty(sim, (BYTE)slots[0], 0) == CMD_OK);
    UT_ASSERT(mkSetDifficulty(sim, (BYTE)slots[1], 1) == CMD_OK);
    UT_ASSERT(mkSetDifficulty(sim, (BYTE)slots[4], 1) == CMD_OK);
    UT_ASSERT(mkBots(sim) == 6);
    mkSnapshot(sim, was);

    /* Seven players onto a map with four starts. Every bot stays, the starts
       go to as many seats as fit, and the rest hold none until a map that
       has room. */
    UT_ASSERT_MSG(mkSetMap(sim, MK_SMALL_MAP) == CMD_OK,
                  "the small map was refused");
    UT_ASSERT_MSG(startsGetNumStarts(&sim->sim.ss) == MK_SMALL_STARTS,
                  "the small map has %d starts, expected %d",
                  (int)startsGetNumStarts(&sim->sim.ss), MK_SMALL_STARTS);
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "onto the small map") != 0) return 1;
    UT_ASSERT_MSG(mkBots(sim) == 6, "%d bots after the small map, expected 6",
                  mkBots(sim));
    if (mkStartsValid(sim, &held, "onto the small map") != 0) return 1;
    UT_ASSERT_MSG(held <= MK_SMALL_STARTS,
                  "%d seats hold a start on a map with %d", held,
                  MK_SMALL_STARTS);

    /* And back onto a map with room for everyone. */
    UT_ASSERT_MSG(mkSetMap(sim, MK_BIG_MAP) == CMD_OK,
                  "the big map was refused the second time");
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "back onto the big map") != 0) return 1;
    if (mkStartsValid(sim, &held, "back onto the big map") != 0) return 1;

    /* The bots are still running, not seats with nothing behind them. */
    for (i = 0; i < 6; i++) {
        UT_ASSERT_MSG(sim->botMgr.bots[slots[i]].active,
                      "%s in seat %d has no bot behind it", kNames[i],
                      slots[i]);
    }

    mkDestroy(sim);
    return 0;
}

/* ── A picked scenario with a lobby of its own ────────────────────── */

int run_lobby_map_keeps_bots_scenario(void) {
    static const char *const kList[] = { "waves.lua", "fastreload.lua" };
    ServerSim *sim;
    MkSeat     was[MAX_TANKS], now[MAX_TANKS];
    int        seat, hostBot, otherBot, raiders, held;
    BYTE       i;

    UT_ASSERT(mkSetUp());
    UT_ASSERT(mkPut(mkScripts, "waves.lua", kMkWaves));
    UT_ASSERT(mkPut(mkScripts, "fastreload.lua", kMkMod));
    ut_brain_stub_arm(true);
    sim = mkSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(mkSetMap(sim, MK_BIG_MAP) == CMD_OK);

    UT_ASSERT_MSG(mkPick(sim, kList, 2) == CMD_OK,
                  "the scenario and the mod were refused");
    raiders = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat &&
            sim->lobbyPlayers[i].teamNumber == 3) {
            raiders++;
        }
    }
    UT_ASSERT_MSG(raiders == 4, "the scenario seated %d, expected its 4",
                  raiders);

    /* What a host does in that lobby: takes one of the scenario's seats off,
       adds a bot to the scenario's team and one to a team of their own, and
       picks a difficulty for one of them. */
    seat = -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat) {
            seat = i;
        }
    }
    UT_ASSERT(seat > 0);
    UT_ASSERT(mkRemoveBot(sim, (BYTE)seat) == CMD_OK);
    hostBot = mkAddBot(sim, "Raider Extra", 3);
    UT_ASSERT_MSG(hostBot > 0, "Add Bot on the scenario's team was refused");
    otherBot = mkAddBot(sim, "Lone Wolf", 1);
    UT_ASSERT_MSG(otherBot > 0, "Add Bot on team 1 was refused");
    UT_ASSERT(mkSetDifficulty(sim, (BYTE)otherBot, 0) == CMD_OK);
    mkSnapshot(sim, was);
    UT_ASSERT(was[hostBot].bot && !was[hostBot].keepSeat);
    UT_ASSERT(was[otherBot].difficulty == 0);

    /* A map with fewer starts than the lobby has players, then back. The
       scenario is the host's pick and not the map's, so it stays, and with
       it every seat exactly as the host left it. */
    UT_ASSERT_MSG(mkSetMap(sim, MK_SMALL_MAP) == CMD_OK,
                  "the small map was refused");
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "onto the small map") != 0) return 1;
    if (mkScriptListIs(sim, kList, 2, "onto the small map") != 0) return 1;
    if (mkStartsValid(sim, &held, "onto the small map") != 0) return 1;
    UT_ASSERT_MSG(strcmp(sim->scenarioIdentity.fileName, "waves.lua") == 0,
                  "the scenario playing over the small map is \"%s\"",
                  sim->scenarioIdentity.fileName);

    UT_ASSERT_MSG(mkSetMap(sim, MK_BIG_MAP) == CMD_OK,
                  "the big map was refused the second time");
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "back onto the big map") != 0) return 1;
    if (mkScriptListIs(sim, kList, 2, "back onto the big map") != 0) return 1;
    UT_ASSERT_MSG(sim->botMgr.bots[hostBot].active &&
                  sim->botMgr.bots[otherBot].active,
                  "a host's bot has nothing behind it after two map changes");

    /* The rule that still takes bots off: a different lobby layout. Picking
       none takes the scenario's lobby away, so its seats go and the host's
       own bots stay, which is what a plain map has always done. */
    UT_ASSERT(mkPick(sim, NULL, 0) == CMD_OK);
    UT_ASSERT_MSG(serverSimIsBot(sim, (BYTE)hostBot) &&
                  serverSimIsBot(sim, (BYTE)otherBot),
                  "the host's own bots went with the scenario's lobby");
    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(!(sim->playerConnected[i] &&
                        sim->lobbyPlayers[i].keepSeat),
                      "seat %d of the scenario outlived it", (int)i);
    }

    mkDestroy(sim);
    return 0;
}

/* ── A map with its own script, and a mod picked over it ──────────── */

int run_lobby_map_keeps_bots_map_own_row(void) {
    static const char *const kMods[] = { "fastreload.lua" };
    ServerSim *sim;
    MkSeat     was[MAX_TANKS], now[MAX_TANKS];
    int        a, b;
    char       ownLeaf[64];

    UT_ASSERT(mkSetUp());
    UT_ASSERT(mkPut(mkScripts, "fastreload.lua", kMkMod));
    /* The big map carries a script beside it; the small one does not. */
    snprintf(ownLeaf, sizeof(ownLeaf), "big%s", SCN_SCRIPT_SUFFIX);
    UT_ASSERT(mkPut(mkMaps, ownLeaf, kMkMapOwn));
    ut_brain_stub_arm(true);
    sim = mkSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(mkSetMap(sim, MK_BIG_MAP) == CMD_OK);
    UT_ASSERT_MSG(serverSimGetMapScript(sim) != NULL,
                  "the big map's own script was not found beside it");

    UT_ASSERT(mkPick(sim, kMods, 1) == CMD_OK);
    a = mkAddBot(sim, "Alpha", 1);
    b = mkAddBot(sim, "Bravo", 2);
    UT_ASSERT(a > 0 && b > 0);
    UT_ASSERT(mkSetDifficulty(sim, (BYTE)b, 1) == CMD_OK);
    mkSnapshot(sim, was);
    /* What the lobby draws: the map's own row at the front, the mod behind
       it. */
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "the lobby draws %d rows, expected the map's and the mod",
                  serverSimGetLobbyScriptCount(sim));

    /* A plain map: the map's row goes because the map that brought it went,
       and the mod the host picked stays. */
    UT_ASSERT(mkSetMap(sim, MK_SMALL_MAP) == CMD_OK);
    UT_ASSERT_MSG(serverSimGetMapScript(sim) == NULL,
                  "the big map's script outlived the big map");
    if (mkScriptListIs(sim, kMods, 1, "onto the plain map") != 0) return 1;
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 1,
                  "the lobby draws %d rows over the plain map, expected the "
                  "mod alone", serverSimGetLobbyScriptCount(sim));
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "onto the plain map") != 0) return 1;

    /* And back: the map's own row returns in front of the mod. */
    UT_ASSERT(mkSetMap(sim, MK_BIG_MAP) == CMD_OK);
    UT_ASSERT_MSG(serverSimGetMapScript(sim) != NULL,
                  "the big map's own script did not come back with it");
    if (mkScriptListIs(sim, kMods, 1, "back onto the big map") != 0) return 1;
    UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                  "the lobby draws %d rows back on the big map, expected 2",
                  serverSimGetLobbyScriptCount(sim));
    mkSnapshot(sim, now);
    if (mkSameRoster(was, now, "back onto the big map") != 0) return 1;

    mkDestroy(sim);
    return 0;
}

/*
 * The scripted game type (test_scripted_game_type.c).
 *
 * gameScripted is the fourth game type. Nothing in the engine has
 * behaviour of its own for it: every site that picks behaviour from the
 * game type sends it through gameTypeResolve, which answers the base game
 * the scenario declared. That value reaches the sim on the lobby template
 * the host hands over, and the sim keeps it as scenarioBaseGame, beside
 * the start a scenario names. Nothing declared leaves it 0, and 0 plays
 * open.
 *
 * The two sites a spawning tank goes through are covered here: the
 * loadout (gameTypeGetItems) and the start (startsGetStart). Both are
 * asserted against what the declared game gives on its own, so the case
 * says "a scripted round is handed what an open round is handed" rather
 * than repeating the amounts.
 *
 * run_scripted_game_type_loadout_follows_base
 *                                — open, tournament and strict each reach
 *                                  a scripted spawn through the mirror
 * run_scripted_game_type_no_base_is_open
 *                                — nothing declared spawns as open
 * run_scripted_game_type_strict_ignores_base
 *                                — a declared base changes no other type
 * run_scripted_game_type_start_follows_base
 *                                — the start picker takes the same path
 * run_scripted_game_type_plain_map_clears_base
 *                                — a plain map after a scripted one keeps
 *                                  no base game type
 * run_scripted_game_type_client_follows_settings
 *                                — and a joined client, which learns the
 *                                  base game off the settings tail
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "gametype.h"
#include "starts.h"              /* startsGetStart */
#include "bolo_rand.h"           /* bolo_srand */
#include "server_sim.h"          /* ut_make_running_sim, serverSimGetGameSim */
#include "server_sim_scenario.h" /* serverSimSetScenarioLobbyTemplate */
#include "client_sim.h"          /* clientSimAlloc, clientSimGetGameSim */
#include "client_sim_control.h"  /* clientSimApplyControl */
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* How many PRNG seeds the start cases walk. Each one is a whole run of the
 * picker, so the two game types are compared on every scan offset rather
 * than on one. */
#define SG_SEEDS 32

/* The four amounts a spawn is handed, so a case can compare two of them. */
typedef struct {
    BYTE shells;
    BYTE mines;
    BYTE armour;
    BYTE trees;
} SgItems;

static void sgItems(GameSim *gs, gameType playing, SgItems *out) {
    gameType asked = playing;

    memset(out, 0, sizeof(*out));
    gameTypeGetItems(gs, &asked, &out->shells, &out->mines, &out->armour,
                     &out->trees);
}

static bool sgSameItems(const SgItems *a, const SgItems *b) {
    return a->shells == b->shells && a->mines == b->mines &&
           a->armour == b->armour && a->trees == b->trees;
}

/* What a scripted round declaring `base` is handed, against what `base`
 * hands a round of its own. */
static int sgLoadoutMatches(GameSim *gs, gameType base) {
    SgItems plain;
    SgItems scripted;

    gs->scenarioBaseGame = base;
    sgItems(gs, base, &plain);
    sgItems(gs, gameScripted, &scripted);

    UT_ASSERT_MSG(sgSameItems(&plain, &scripted),
                  "a scripted round declaring game type %d was handed "
                  "%u/%u/%u/%u (shells/mines/armour/trees), and that game "
                  "hands out %u/%u/%u/%u",
                  (int)base,
                  (unsigned)scripted.shells, (unsigned)scripted.mines,
                  (unsigned)scripted.armour, (unsigned)scripted.trees,
                  (unsigned)plain.shells, (unsigned)plain.mines,
                  (unsigned)plain.armour, (unsigned)plain.trees);
    return 0;
}

/* ── The loadout follows the declared base game ───────────────────── */

int run_scripted_game_type_loadout_follows_base(void) {
    ServerSim *sim = ut_make_running_sim("Scripted");
    GameSim   *gs;
    SgItems    strict;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    if (sgLoadoutMatches(gs, gameOpen) != 0)             return 1;
    if (sgLoadoutMatches(gs, gameTournament) != 0)       return 1;
    if (sgLoadoutMatches(gs, gameStrictTournament) != 0) return 1;

    /* And the strict amounts are what strict has always been, so the case
       above is not comparing two wrong answers: nothing but armour. */
    gs->scenarioBaseGame = gameStrictTournament;
    sgItems(gs, gameScripted, &strict);
    UT_ASSERT_MSG(strict.shells == 0 && strict.mines == 0 && strict.trees == 0,
                  "a scripted round declaring strict got %u shells, %u mines "
                  "and %u trees, expected none of any",
                  (unsigned)strict.shells, (unsigned)strict.mines,
                  (unsigned)strict.trees);
    UT_ASSERT_MSG(strict.armour == (BYTE)gs->rules.tank_full_armour,
                  "a scripted round declaring strict got %u armour, expected "
                  "the rule's %ld",
                  (unsigned)strict.armour, (long)gs->rules.tank_full_armour);

    serverSimDestroy(sim);
    return 0;
}

/* ── Nothing declared plays open ──────────────────────────────────── */

int run_scripted_game_type_no_base_is_open(void) {
    ServerSim *sim = ut_make_running_sim("Scripted");
    GameSim   *gs;
    SgItems    open;
    SgItems    scripted;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    sgItems(gs, gameOpen, &open);
    gs->scenarioBaseGame = (gameType)0;
    sgItems(gs, gameScripted, &scripted);

    UT_ASSERT_MSG(sgSameItems(&open, &scripted),
                  "a scripted round declaring nothing was handed %u shells, "
                  "%u mines and %u trees; an open round is handed %u/%u/%u",
                  (unsigned)scripted.shells, (unsigned)scripted.mines,
                  (unsigned)scripted.trees,
                  (unsigned)open.shells, (unsigned)open.mines,
                  (unsigned)open.trees);
    /* Pinned as amounts too, so a change that emptied both would be seen. */
    UT_ASSERT_MSG(scripted.shells == (BYTE)gs->rules.tank_full_shells,
                  "a scripted round declaring nothing got %u shells, expected "
                  "the open game's %ld",
                  (unsigned)scripted.shells, (long)gs->rules.tank_full_shells);

    serverSimDestroy(sim);
    return 0;
}

/* ── A declared base changes nothing else ─────────────────────────── */

int run_scripted_game_type_strict_ignores_base(void) {
    ServerSim *sim = ut_make_running_sim("Scripted");
    GameSim   *gs;
    SgItems    strict;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    /* A strict round on a sim that still carries a scenario's declared
       base game is strict, not open: only gameScripted reads the mirror. */
    gs->scenarioBaseGame = gameOpen;
    sgItems(gs, gameStrictTournament, &strict);

    UT_ASSERT_MSG(strict.shells == 0 && strict.mines == 0 && strict.trees == 0,
                  "a strict round beside a declared open base game got "
                  "%u shells, %u mines and %u trees, expected none of any",
                  (unsigned)strict.shells, (unsigned)strict.mines,
                  (unsigned)strict.trees);

    serverSimDestroy(sim);
    return 0;
}

/* ── The start picker takes the declared game's path ──────────────── */

/* One run of the picker for `playing`, at seed `seed`. */
static void sgStartAt(GameSim *gs, gameType playing, gameType base,
                      uint64_t seed, BYTE *x, BYTE *y, TURNTYPE *dir) {
    gs->game             = playing;
    gs->scenarioBaseGame = base;
    gs->pendingStartIdx[0] = MAX_STARTS;   /* leave the picker to choose */
    *x = 0;
    *y = 0;
    *dir = 0;
    bolo_srand(seed);
    startsGetStart(gs, &gs->ss, x, y, dir, 0);
}

/* Every seed picks the same square for `base` played plainly and for a
 * scripted round declaring it. */
static int sgStartsMatch(GameSim *gs, gameType base) {
    uint64_t seed;

    for (seed = 1; seed <= SG_SEEDS; seed++) {
        BYTE     px = 0, py = 0, sx = 0, sy = 0;
        TURNTYPE pd = 0, sd = 0;

        sgStartAt(gs, base, base, seed, &px, &py, &pd);
        sgStartAt(gs, gameScripted, base, seed, &sx, &sy, &sd);
        UT_ASSERT_MSG(px == sx && py == sy && pd == sd,
                      "seed %lu: a scripted round declaring game type %d "
                      "started at %u,%u and that game starts at %u,%u",
                      (unsigned long)seed, (int)base,
                      (unsigned)sx, (unsigned)sy, (unsigned)px, (unsigned)py);
    }
    return 0;
}

int run_scripted_game_type_start_follows_base(void) {
    ServerSim *sim = ut_make_running_sim("Scripted");
    GameSim   *gs;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->ss != NULL);

    if (sgStartsMatch(gs, gameOpen) != 0)             return 1;
    if (sgStartsMatch(gs, gameStrictTournament) != 0) return 1;

    serverSimDestroy(sim);
    return 0;
}

/* ── A plain map after a scripted one keeps no base game type ─────── */

int run_scripted_game_type_plain_map_clears_base(void) {
    ServerSim       *sim = ut_make_running_sim("Scripted");
    GameSim         *gs;
    ScnLobbyTemplate t;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    UT_ASSERT_MSG(gs->scenarioBaseGame == (gameType)0,
                  "a fresh sim already carries base game type %d",
                  (int)gs->scenarioBaseGame);

    /* The host hands over a scenario that declared "strict". */
    memset(&t, 0, sizeof(t));
    t.baseGameType = (uint8_t)gameStrictTournament;
    serverSimSetScenarioLobbyTemplate(sim, &t);
    UT_ASSERT_MSG(gs->scenarioBaseGame == gameStrictTournament,
                  "the template's base game type reached the sim as %d, "
                  "expected %d",
                  (int)gs->scenarioBaseGame, (int)gameStrictTournament);

    /* A plain map is committed: whoever owned the scenario detaches and
       clears the template. */
    serverSimSetScenarioLobbyTemplate(sim, NULL);
    UT_ASSERT_MSG(gs->scenarioBaseGame == (gameType)0,
                  "a plain map left the previous scenario's base game type "
                  "%d behind",
                  (int)gs->scenarioBaseGame);

    /* And a scripted spawn on it is open again. */
    {
        SgItems open;
        SgItems scripted;
        sgItems(gs, gameOpen, &open);
        sgItems(gs, gameScripted, &scripted);
        UT_ASSERT_MSG(sgSameItems(&open, &scripted),
                      "after a plain map a scripted spawn got %u shells, "
                      "expected the open game's %u",
                      (unsigned)scripted.shells, (unsigned)open.shells);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── A joined client resolves the same way ────────────────────────── */

/* No client is ever handed a lobby template, so the settings tail is the
 * only place one learns the game underneath a scripted round. The event
 * goes through the codec body here, so what the client is given is what the
 * wire carries: a body with the tail, then a body without one. */
int run_scripted_game_type_client_follows_settings(void) {
    ControlEncodeBodyFn benc =
        transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    uint8_t      body[MAX_CONTROL_PACKET];
    size_t       bodyLen = 0;
    ControlEvent in;
    ControlEvent out;
    ClientSim   *cs;
    GameSim     *gs;
    SgItems      scripted;
    SgItems      strict;
    SgItems      open;

    UT_ASSERT(benc != NULL && bdec != NULL);
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(gs->scenarioBaseGame == (gameType)0,
                  "a fresh client already carries base game type %d",
                  (int)gs->scenarioBaseGame);

    /* The settings a server running a scripted map that declared strict
       sends: the lobby type is gameScripted for the whole of the round, and
       the base game rides the scenario tail behind it. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    snprintf(in.u.lobbySettings.mapName,
             sizeof(in.u.lobbySettings.mapName), "%s", "ScriptedMap");
    in.u.lobbySettings.lobbyGameType    = gameScripted;
    in.u.lobbySettings.scenarioSource   = lobbyScenarioMap;
    snprintf(in.u.lobbySettings.scenarioName,
             sizeof(in.u.lobbySettings.scenarioName), "%s", "Wave");
    in.u.lobbySettings.scenarioBaseGame = (uint8_t)gameStrictTournament;

    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &bodyLen) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, bodyLen, &out),
                  "the scripted settings body did not decode");
    clientSimApplyControl(cs, &out);

    UT_ASSERT_MSG(gs->scenarioBaseGame == gameStrictTournament,
                  "the base game off the settings tail reached the client as "
                  "%d, wanted %d",
                  (int)gs->scenarioBaseGame, (int)gameStrictTournament);

    /* The loadout this client would predict for its first life, asked the
       way a spawn asks: gameScripted in, the declared game's amounts out. */
    sgItems(gs, gameScripted, &scripted);
    sgItems(gs, gameStrictTournament, &strict);
    UT_ASSERT_MSG(sgSameItems(&scripted, &strict),
                  "a client on a scripted round declaring strict predicted "
                  "%u/%u/%u/%u (shells/mines/armour/trees), and strict hands "
                  "out %u/%u/%u/%u",
                  (unsigned)scripted.shells, (unsigned)scripted.mines,
                  (unsigned)scripted.armour, (unsigned)scripted.trees,
                  (unsigned)strict.shells, (unsigned)strict.mines,
                  (unsigned)strict.armour, (unsigned)strict.trees);
    /* And not the open amounts it would have predicted with nothing on the
       tail — the two differ, so the case above is not passing by accident. */
    sgItems(gs, gameOpen, &open);
    UT_ASSERT_MSG(!sgSameItems(&scripted, &open),
                  "strict and open hand out the same %u shells here, so this "
                  "case cannot tell them apart",
                  (unsigned)open.shells);

    /* The same client told about a plain map. That body carries no scenario
       tail at all, so the base game has to go back to 0 and the prediction
       back to open. */
    {
        ControlEvent plainIn;
        ControlEvent plainOut;
        size_t       plainLen = 0;

        memset(&plainIn, 0, sizeof(plainIn));
        plainIn.type = CTRL_LOBBY_SETTINGS;
        snprintf(plainIn.u.lobbySettings.mapName,
                 sizeof(plainIn.u.lobbySettings.mapName), "%s", "PlainMap");
        plainIn.u.lobbySettings.lobbyGameType = gameOpen;
        UT_ASSERT(benc(&plainIn, NULL, body, sizeof(body), &plainLen) ==
                  ENCODE_OK);
        UT_ASSERT_MSG(plainLen < bodyLen,
                      "the plain body came to %u bytes and the scripted one "
                      "to %u — the plain body must carry no tail",
                      (unsigned)plainLen, (unsigned)bodyLen);
        memset(&plainOut, 0, sizeof(plainOut));
        UT_ASSERT(bdec(body, plainLen, &plainOut));
        clientSimApplyControl(cs, &plainOut);

        UT_ASSERT_MSG(gs->scenarioBaseGame == (gameType)0,
                      "a plain map left the base game type %d on the client",
                      (int)gs->scenarioBaseGame);
        sgItems(gs, gameScripted, &scripted);
        sgItems(gs, gameOpen, &open);
        UT_ASSERT_MSG(sgSameItems(&scripted, &open),
                      "after a plain map the client predicted %u shells for a "
                      "scripted round, expected the open game's %u",
                      (unsigned)scripted.shells, (unsigned)open.shells);
    }

    clientSimDestroy(cs);
    return 0;
}

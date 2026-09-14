/*
 * What the lobby says about a scenario, and what a scripted map does to the
 * lobby's own settings.
 *
 * CTRL_LOBBY_SETTINGS grew a scenario tail: the source, the name, the file
 * it came from, the description and the extra-teams flag. The tail is
 * written only when there is a scenario, so a lobby with none puts exactly
 * the bytes on the wire it always did — which is what the golden case below
 * is for. A round trip alone would pass whenever the encoder and the decoder
 * agreed with each other, so both shapes are compared against bytes written
 * out here by hand and never read back through the codec.
 *
 * The commit cases stand a sim up with an identity set by hand rather than
 * through a Lua host: with nothing registered to answer the map change, the
 * identity the case set is what the commit path reads, which is the surface
 * under test.
 *
 * run_lobby_scenario_settings_plain_bytes  — the no-scenario body, byte for
 *                                            byte, and its length
 * run_lobby_scenario_settings_scripted_bytes
 *                                          — the same base plus the tail
 * run_lobby_scenario_settings_roundtrip    — every scenario field survives
 * run_lobby_scenario_commit_sets_type      — scripted in, plain out
 * run_lobby_scenario_refuses_ranked        — both directions
 * run_lobby_scenario_refuses_ai_none       — and the policy that empties the
 *                                            roster of bots
 * run_lobby_scenario_refuses_game_type     — and the type the commit set
 * run_lobby_scenario_boot_sets_type        — and the same type on a server
 *                                            that booted onto the map
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"  /* serverSimApplyLobbySetting, SetRanked */
#include "server_sim_scenario.h"   /* serverSimSetScenarioIdentity */
#include "mapgen.h"                /* MapGenConfig — the generated-map commit */
#include "wire_limits.h"           /* LST_RANKED, LST_AI_POLICY, LST_GAME_TYPE */
#include "everard_map.h"
#include "test_harness.h"

/* What the body was before a scenario had anything to say, written out
 * rather than taken from the codec's own macro: a field added to the event
 * without a thought about the lobby that has no scenario fails here and is
 * looked at.
 *
 *   mapName MAP_STR_SIZE(36)
 * + gameType 1 + hiddenMines 1 + aiType 1 + timeLimit 4
 * + pill 1 + base 1 + start 1 + mapSkip 1 + netStat 1 + hasLobby 1
 * + openHost 1 + autoLock 1 + serverLocks 4                     = 55
 * + ranked 1 + allowNew 1 + wbn 1 + uploadPolicy 1 + startDelay 4
 * + hostSlot 1 + viewPolicy 3 + viewDecay 6 + classic 1
 * + alliesInTrees 1 + voice 1 + overview 1 + lineOfSight 1      = 23
 */
#define LS_PLAIN_BODY_LEN 78

/* The three strings the scripted cases carry, and what each costs on the
 * wire: a one-byte length and that many bytes, no terminator. */
#define LS_SCN_NAME "Wave"
#define LS_SCN_FILE "wave.lua"
#define LS_SCN_DESC "Hold out"
#define LS_SCN_NAME_LEN 4
#define LS_SCN_FILE_LEN 8
#define LS_SCN_DESC_LEN 8
#define LS_SCRIPTED_BODY_LEN                                                \
    (LS_PLAIN_BODY_LEN + 1 + 1 + (1 + LS_SCN_NAME_LEN)                      \
     + (1 + LS_SCN_FILE_LEN) + (1 + LS_SCN_DESC_LEN))

/* Distinct values throughout, so a pair of fields swapped between the event
 * and the bytes shows up as two mismatches rather than cancelling out. The
 * wide fields take values whose bytes differ, so a byte order that flipped
 * would be seen. */
static void lsFillPlain(ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_LOBBY_SETTINGS;
    snprintf(evt->u.lobbySettings.mapName,
             sizeof(evt->u.lobbySettings.mapName), "%s", "Golden");
    evt->u.lobbySettings.lobbyGameType            = gameTournament;
    evt->u.lobbySettings.lobbyHiddenMines         = true;
    evt->u.lobbySettings.lobbyAiType              = 3;
    evt->u.lobbySettings.lobbyTimeLimit           = 0x01020304;
    evt->u.lobbySettings.lobbyPillCount           = 11;
    evt->u.lobbySettings.lobbyBaseCount           = 7;
    evt->u.lobbySettings.lobbyStartCount          = 9;
    evt->u.lobbySettings.mapSkipAvailable         = true;
    evt->u.lobbySettings.netStat                  = netRunning;
    evt->u.lobbySettings.hasLobby                 = true;
    evt->u.lobbySettings.lobbyOpenHost            = true;
    evt->u.lobbySettings.lobbyAutoLockOnGameStart = true;
    evt->u.lobbySettings.lobbyServerLocks         = 0xAABBCCDDu;
    evt->u.lobbySettings.lobbyRanked              = false;
    evt->u.lobbySettings.lobbyAllowNewPlayers     = true;
    evt->u.lobbySettings.lobbyWbnAvailable        = true;
    evt->u.lobbySettings.uploadPolicy             = (UploadPolicy)2;
    evt->u.lobbySettings.lobbyStartDelay          = 0x00000100;
    evt->u.lobbySettings.hostSlot                 = 5;
    evt->u.lobbySettings.viewPolicy[0]            = (ViewPolicy)1;
    evt->u.lobbySettings.viewPolicy[1]            = (ViewPolicy)2;
    evt->u.lobbySettings.viewPolicy[2]            = (ViewPolicy)3;
    evt->u.lobbySettings.viewDecaySecs[0]         = 0x0102;
    evt->u.lobbySettings.viewDecaySecs[1]         = 0x0304;
    evt->u.lobbySettings.viewDecaySecs[2]         = 0x0506;
    evt->u.lobbySettings.lobbyClassicMode         = true;
    evt->u.lobbySettings.lobbyAlliesInTrees       = true;
    evt->u.lobbySettings.voiceMode                = serverVoiceOff;
    evt->u.lobbySettings.lobbyOverviewWindow      = 2;
    evt->u.lobbySettings.lobbyLineOfSight         = 1;
}

/* The same bytes the fill above should produce, written out by index. Every
 * offset is a literal, so a field that moves moves here too or the case
 * fails. */
static void lsWantPlain(uint8_t *want) {
    memset(want, 0, LS_PLAIN_BODY_LEN);
    memcpy(want, "Golden", 6);      /* [0..35] mapName, zero-padded */
    want[36] = (uint8_t)gameTournament;
    want[37] = 1;                   /* hiddenMines */
    want[38] = 3;                   /* aiType */
    want[39] = 0x01;                /* timeLimit, big-endian */
    want[40] = 0x02;
    want[41] = 0x03;
    want[42] = 0x04;
    want[43] = 11;                  /* pillCount */
    want[44] = 7;                   /* baseCount */
    want[45] = 9;                   /* startCount */
    want[46] = 1;                   /* mapSkipAvailable */
    want[47] = (uint8_t)netRunning;
    want[48] = 1;                   /* hasLobby */
    want[49] = 1;                   /* openHost */
    want[50] = 1;                   /* autoLockOnGameStart */
    want[51] = 0xAA;                /* serverLocks, big-endian */
    want[52] = 0xBB;
    want[53] = 0xCC;
    want[54] = 0xDD;
    want[55] = 0;                   /* ranked */
    want[56] = 1;                   /* allowNewPlayers */
    want[57] = 1;                   /* wbnAvailable */
    want[58] = 2;                   /* uploadPolicy */
    want[59] = 0x00;                /* startDelay, big-endian */
    want[60] = 0x00;
    want[61] = 0x01;
    want[62] = 0x00;
    want[63] = 5;                   /* hostSlot */
    want[64] = 1;                   /* viewPolicy[0] */
    want[65] = 2;
    want[66] = 3;
    want[67] = 0x01;                /* viewDecaySecs[0], big-endian */
    want[68] = 0x02;
    want[69] = 0x03;                /* viewDecaySecs[1] */
    want[70] = 0x04;
    want[71] = 0x05;                /* viewDecaySecs[2] */
    want[72] = 0x06;
    want[73] = 1;                   /* classicMode */
    want[74] = 1;                   /* alliesInTrees */
    want[75] = (uint8_t)serverVoiceOff;
    want[76] = 2;                   /* overviewWindow */
    want[77] = 1;                   /* lineOfSight */
}

/* Where the first byte that differs is, or -1 when they match. */
static int lsFirstDiff(const uint8_t *got, const uint8_t *want, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) return (int)i;
    }
    return -1;
}

/* ── 1. The body a lobby with no scenario writes ──────────────────── */

int run_lobby_scenario_settings_plain_bytes(void) {
    ControlEvent        in;
    ControlEncodeBodyFn benc = transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    uint8_t             got[MAX_CONTROL_PACKET];
    uint8_t             want[LS_PLAIN_BODY_LEN];
    size_t              gotLen = 0;
    int                 at;

    UT_ASSERT(benc != NULL);
    lsFillPlain(&in);
    UT_ASSERT_MSG(in.u.lobbySettings.scenarioSource == lobbyScenarioNone,
                  "setup: the plain event must carry no scenario");
    UT_ASSERT(benc(&in, NULL, got, sizeof(got), &gotLen) == ENCODE_OK);

    UT_ASSERT_MSG(gotLen == LS_PLAIN_BODY_LEN,
                  "a lobby with no scenario wrote %u body bytes, and the "
                  "body has always been %d — a plain map's settings must not "
                  "change length",
                  (unsigned)gotLen, LS_PLAIN_BODY_LEN);

    lsWantPlain(want);
    at = lsFirstDiff(got, want, LS_PLAIN_BODY_LEN);
    UT_ASSERT_MSG(at < 0,
                  "byte %d of the no-scenario body is 0x%02X, expected 0x%02X",
                  at, (unsigned)got[at < 0 ? 0 : at],
                  (unsigned)want[at < 0 ? 0 : at]);
    return 0;
}

/* ── 2. The body a lobby with one writes ──────────────────────────── */

int run_lobby_scenario_settings_scripted_bytes(void) {
    ControlEvent        in;
    ControlEncodeBodyFn benc = transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    uint8_t             got[MAX_CONTROL_PACKET];
    uint8_t             want[LS_SCRIPTED_BODY_LEN];
    size_t              gotLen = 0;
    size_t              pos;
    int                 at;

    UT_ASSERT(benc != NULL);
    lsFillPlain(&in);
    in.u.lobbySettings.scenarioSource = lobbyScenarioMap;
    snprintf(in.u.lobbySettings.scenarioName,
             sizeof(in.u.lobbySettings.scenarioName), "%s", LS_SCN_NAME);
    snprintf(in.u.lobbySettings.scenarioFileName,
             sizeof(in.u.lobbySettings.scenarioFileName), "%s", LS_SCN_FILE);
    snprintf(in.u.lobbySettings.scenarioDescription,
             sizeof(in.u.lobbySettings.scenarioDescription), "%s", LS_SCN_DESC);
    in.u.lobbySettings.scenarioExtraTeams = true;

    UT_ASSERT(benc(&in, NULL, got, sizeof(got), &gotLen) == ENCODE_OK);
    UT_ASSERT_MSG(gotLen == LS_SCRIPTED_BODY_LEN,
                  "a lobby with a scenario wrote %u body bytes, expected %d",
                  (unsigned)gotLen, LS_SCRIPTED_BODY_LEN);

    /* The base is the same bytes as before, and the tail follows it. */
    memset(want, 0, sizeof(want));
    lsWantPlain(want);
    pos = LS_PLAIN_BODY_LEN;
    want[pos++] = (uint8_t)lobbyScenarioMap;
    want[pos++] = 1;                        /* extraTeams */
    want[pos++] = LS_SCN_NAME_LEN;
    memcpy(want + pos, LS_SCN_NAME, LS_SCN_NAME_LEN);
    pos += LS_SCN_NAME_LEN;
    want[pos++] = LS_SCN_FILE_LEN;
    memcpy(want + pos, LS_SCN_FILE, LS_SCN_FILE_LEN);
    pos += LS_SCN_FILE_LEN;
    want[pos++] = LS_SCN_DESC_LEN;
    memcpy(want + pos, LS_SCN_DESC, LS_SCN_DESC_LEN);
    pos += LS_SCN_DESC_LEN;
    UT_ASSERT_MSG(pos == LS_SCRIPTED_BODY_LEN,
                  "the case's own expected bytes came to %u, not %d",
                  (unsigned)pos, LS_SCRIPTED_BODY_LEN);

    at = lsFirstDiff(got, want, LS_SCRIPTED_BODY_LEN);
    UT_ASSERT_MSG(at < 0,
                  "byte %d of the scripted body is 0x%02X, expected 0x%02X",
                  at, (unsigned)got[at < 0 ? 0 : at],
                  (unsigned)want[at < 0 ? 0 : at]);
    return 0;
}

/* ── 3. Every scenario field survives the trip ────────────────────── */

int run_lobby_scenario_settings_roundtrip(void) {
    ControlEvent        in, out;
    ControlEncodeBodyFn benc = transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec = transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    uint8_t             body[MAX_CONTROL_PACKET];
    size_t              bodyLen = 0;

    UT_ASSERT(benc != NULL && bdec != NULL);
    lsFillPlain(&in);
    in.u.lobbySettings.scenarioSource = lobbyScenarioMap;
    snprintf(in.u.lobbySettings.scenarioName,
             sizeof(in.u.lobbySettings.scenarioName), "%s", LS_SCN_NAME);
    snprintf(in.u.lobbySettings.scenarioFileName,
             sizeof(in.u.lobbySettings.scenarioFileName), "%s", LS_SCN_FILE);
    snprintf(in.u.lobbySettings.scenarioDescription,
             sizeof(in.u.lobbySettings.scenarioDescription), "%s", LS_SCN_DESC);
    in.u.lobbySettings.scenarioExtraTeams = true;

    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &bodyLen) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, bodyLen, &out), "the scripted body did not decode");

    UT_ASSERT_MSG(out.u.lobbySettings.scenarioSource == lobbyScenarioMap,
                  "the source came back as %d, wanted %d",
                  (int)out.u.lobbySettings.scenarioSource,
                  (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioName, LS_SCN_NAME) == 0,
                  "the name came back \"%s\"", out.u.lobbySettings.scenarioName);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioFileName, LS_SCN_FILE) == 0,
                  "the file name came back \"%s\"",
                  out.u.lobbySettings.scenarioFileName);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioDescription,
                         LS_SCN_DESC) == 0,
                  "the description came back \"%s\"",
                  out.u.lobbySettings.scenarioDescription);
    UT_ASSERT_MSG(out.u.lobbySettings.scenarioExtraTeams,
                  "the extra-teams flag did not survive");
    /* And the fields ahead of the tail are still themselves. */
    UT_ASSERT(out.u.lobbySettings.lobbyLineOfSight == 1);
    UT_ASSERT(out.u.lobbySettings.voiceMode == serverVoiceOff);
    UT_ASSERT(out.u.lobbySettings.lobbyServerLocks == 0xAABBCCDDu);

    /* A body that stops where a plain one stops leaves no scenario behind. */
    {
        ControlEvent plainIn, plainOut;
        size_t       plainLen = 0;
        lsFillPlain(&plainIn);
        UT_ASSERT(benc(&plainIn, NULL, body, sizeof(body), &plainLen) == ENCODE_OK);
        memset(&plainOut, 0, sizeof(plainOut));
        UT_ASSERT(bdec(body, plainLen, &plainOut));
        UT_ASSERT_MSG(plainOut.u.lobbySettings.scenarioSource ==
                          lobbyScenarioNone,
                      "a no-scenario body decoded to source %d",
                      (int)plainOut.u.lobbySettings.scenarioSource);
        UT_ASSERT_MSG(plainOut.u.lobbySettings.scenarioName[0] == '\0',
                      "a no-scenario body left a name behind");
    }
    return 0;
}

/* ── The sim side ─────────────────────────────────────────────────── */

/* A lobby nobody is in, on the inbuilt map. No scenario host is registered,
 * so the map change asks nothing and the identity a case sets by hand is
 * what the commit path reads. */
static ServerSim *lsLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static void lsAttachIdentity(ServerSim *sim) {
    serverSimSetScenarioIdentity(sim, lobbyScenarioMap, LS_SCN_NAME,
                                 LS_SCN_FILE, LS_SCN_DESC, true);
}

/* Commit a map. Which map does not matter here — what matters is that the
 * commit path runs, so a generated one saves the case a file of its own. */
static bool lsCommitMap(ServerSim *sim) {
    MapGenConfig cfg = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
    return serverSimReloadRandomMap(sim, &cfg);
}

/* ── 4. Scripted in, plain out ────────────────────────────────────── */

int run_lobby_scenario_commit_sets_type(void) {
    ServerSim *sim = lsLobbySim();

    UT_ASSERT(sim != NULL);
    /* A host has picked tournament by hand. That, and not the type the
       operator configured the server with, is what a plain map has to give
       back — the sim was created as gameOpen, so the two differ here and the
       case can tell which one came back. */
    serverSimSetGameType(sim, gameTournament);
    UT_ASSERT_MSG(sim->originalLobbySettings.gameType != gameTournament,
                  "setup: the operator's type and the host's pick must differ "
                  "for this case to mean anything");

    lsAttachIdentity(sim);
    UT_ASSERT_MSG(lsCommitMap(sim), "the scripted commit was refused");
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "a commit with a scenario attached left the game type at %d",
                  (int)serverSimGetGameType(sim));

    /* A second scripted commit must not forget what the lobby was on. */
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT(serverSimGetGameType(sim) == gameScripted);

    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false);
    UT_ASSERT_MSG(lsCommitMap(sim), "the plain commit was refused");
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameTournament,
                  "a plain map gave back game type %d, wanted the host's "
                  "tournament (%d); the operator's own is %d",
                  (int)serverSimGetGameType(sim), (int)gameTournament,
                  (int)sim->originalLobbySettings.gameType);

    /* And a plain commit on a lobby that never had a scenario changes
       nothing. */
    serverSimSetGameType(sim, gameStrictTournament);
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameStrictTournament,
                  "a plain commit moved the game type to %d on its own",
                  (int)serverSimGetGameType(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ── 5. Ranked and a scenario, from both directions ───────────────── */

int run_lobby_scenario_refuses_ranked(void) {
    ServerSim *sim = lsLobbySim();
    uint8_t    on = 1;

    UT_ASSERT(sim != NULL);

    /* With no scenario, ranked goes on as it always has. */
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_RANKED, &on, 1),
                  "ranked was refused on a lobby with no scenario");
    UT_ASSERT(serverSimGetRanked(sim));

    /* A commit that attaches one takes it away. */
    lsAttachIdentity(sim);
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT_MSG(!serverSimGetRanked(sim),
                  "a commit that attached a scenario left the lobby ranked");

    /* And it cannot be put back while the scenario is there. */
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_RANKED, &on, 1),
                  "ranked was accepted on a lobby running a scenario");
    UT_ASSERT_MSG(!serverSimGetRanked(sim),
                  "a refused ranked setting turned ranked on anyway");

    /* With the scenario gone it is the host's again. */
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false);
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_RANKED, &on, 1),
                  "ranked stayed refused after the scenario went");
    UT_ASSERT(serverSimGetRanked(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ── 6. The AI policy that empties the roster of bots ─────────────── */

int run_lobby_scenario_refuses_ai_none(void) {
    ServerSim *sim = lsLobbySim();
    uint8_t    v;

    UT_ASSERT(sim != NULL);

    /* A lobby with no scenario may still be told to run no bots. */
    v = (uint8_t)aiNone;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_AI_POLICY, &v, 1),
                  "aiNone was refused on a lobby with no scenario");
    UT_ASSERT(serverSimGetBotAiType(sim) == aiNone);

    /* A commit that attaches a scenario moves it off aiNone, because that
       setting is the one that takes every bot off the roster. */
    lsAttachIdentity(sim);
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) != aiNone,
                  "a commit with a scenario left the lobby running no bots");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiYes,
                  "the commit moved the AI policy to %d, wanted aiYes (%d)",
                  (int)serverSimGetBotAiType(sim), (int)aiYes);

    /* And it cannot be put back to aiNone while the scenario is there. */
    v = (uint8_t)aiNone;
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_AI_POLICY, &v, 1),
                  "aiNone was accepted on a lobby running a scenario");
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiYes,
                  "a refused AI policy changed the policy anyway, to %d",
                  (int)serverSimGetBotAiType(sim));

    /* The other policies are still the host's to pick between. */
    v = (uint8_t)aiFull;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_AI_POLICY, &v, 1),
                  "aiFull was refused on a lobby running a scenario");
    UT_ASSERT(serverSimGetBotAiType(sim) == aiFull);

    /* A commit leaves a policy that already allows bots alone. */
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT_MSG(serverSimGetBotAiType(sim) == aiFull,
                  "a commit moved a policy that already allowed bots, to %d",
                  (int)serverSimGetBotAiType(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ── 7. The game type a scripted round is on ──────────────────────── */

int run_lobby_scenario_refuses_game_type(void) {
    ServerSim *sim = lsLobbySim();
    /* gameOpen, gameTournament, gameStrictTournament — every value the
       handler admits, each of which would take a scripted lobby off
       gameScripted. */
    const uint8_t types[3] = { (uint8_t)gameOpen, (uint8_t)gameTournament,
                               (uint8_t)gameStrictTournament };
    int i;

    UT_ASSERT(sim != NULL);

    /* With no scenario the three are the host's to pick between. */
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_GAME_TYPE,
                                                 &types[i], 1),
                      "game type %d was refused on a lobby with no scenario",
                      (int)types[i]);
        UT_ASSERT_MSG(serverSimGetGameType(sim) == (gameType)types[i],
                      "game type %d was accepted but the lobby is on %d",
                      (int)types[i], (int)serverSimGetGameType(sim));
    }

    /* A commit that attaches a scenario puts the lobby on scripted. */
    lsAttachIdentity(sim);
    UT_ASSERT(lsCommitMap(sim));
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "a commit with a scenario left the game type at %d",
                  (int)serverSimGetGameType(sim));

    /* And none of the three can take it off scripted while it is there. */
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_GAME_TYPE,
                                                  &types[i], 1),
                      "game type %d was accepted on a lobby running a scenario",
                      (int)types[i]);
        UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                      "a refused game type moved the lobby to %d",
                      (int)serverSimGetGameType(sim));
    }

    /* With the scenario gone the type is the host's again. */
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false);
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_GAME_TYPE, &types[1], 1),
                  "the game type stayed refused after the scenario went");
    UT_ASSERT(serverSimGetGameType(sim) == gameTournament);

    serverSimDestroy(sim);
    return 0;
}

/* ── 8. The server that booted straight onto a scripted map ───────── */

int run_lobby_scenario_boot_sets_type(void) {
    ServerSim *sim = lsLobbySim();

    UT_ASSERT(sim != NULL);

    /* The boot shape: the sim is built and the identity attached, and no map
       is ever committed. That is the whole of what a fresh boot does, and
       before the rules were callable on their own the commit was the only
       thing that reached them.

       The type the sim is put on here stands in for the operator's
       -gametype. Tournament rather than the gameOpen lsLobbySim() builds, so
       the case can tell the remembered type from the default. */
    serverSimSetGameType(sim, gameTournament);
    lsAttachIdentity(sim);
    serverSimScenarioApplyLobbyRules(sim);

    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameScripted,
                  "a boot onto a scripted map left the game type at %d",
                  (int)serverSimGetGameType(sim));
    /* Remembered with no special case for this path: the type the lobby was
       on is the operator's, and that is exactly what a plain map committed
       later has to give back. */
    UT_ASSERT_MSG(sim->preScenarioGameType == gameTournament,
                  "the boot remembered %d as the displaced type, wanted "
                  "tournament (%d)",
                  (int)sim->preScenarioGameType, (int)gameTournament);

    /* And the give-back works from here as it does after a commit. */
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false);
    UT_ASSERT_MSG(lsCommitMap(sim), "the plain commit was refused");
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameTournament,
                  "a plain commit after a boot gave back game type %d, "
                  "wanted the operator's tournament (%d)",
                  (int)serverSimGetGameType(sim), (int)gameTournament);

    serverSimDestroy(sim);

    /* The same boot on a map with no scenario leaves the lobby alone. The
       boot sites make the call whenever the process has a scenario library,
       so a plain map reaches it with nothing attached. */
    sim = lsLobbySim();
    UT_ASSERT(sim != NULL);
    serverSimSetGameType(sim, gameStrictTournament);
    serverSimScenarioApplyLobbyRules(sim);
    UT_ASSERT_MSG(serverSimGetGameType(sim) == gameStrictTournament,
                  "a boot onto a plain map moved the game type to %d",
                  (int)serverSimGetGameType(sim));
    UT_ASSERT_MSG(sim->preScenarioGameType == (gameType)0,
                  "a boot onto a plain map remembered %d as a displaced type",
                  (int)sim->preScenarioGameType);

    serverSimDestroy(sim);
    return 0;
}

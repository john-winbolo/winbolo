/*
 * Codec round-trip + apply-path coverage for the lobby ControlEvent
 * variants this branch added or extended:
 *
 *   CTRL_LOBBY_SETTINGS   — extended with lobbyOpenHost,
 *                            lobbyAutoLockOnGameStart, lobbyServerLocks
 *   CTRL_LOBBY_TEAM_META  — new
 *   CTRL_LOBBY_BOT_CONFIG — new
 *   CTRL_LOBBY_BOT_BRAIN  — new
 *   CTRL_LOBBY_BRAIN_LIST — new
 *
 * Each test fills a ControlEvent, runs it through the encoder table
 * (transportControlCodecEncoder), recovers it via the decoder table
 * keyed by the encoded packet's wire type, and asserts every field
 * survives. Then it builds a fresh ClientSim, dispatches the same
 * event through clientSimApplyControl, and asserts the matching
 * ClientSim mirror fields. The two halves test different surfaces:
 * the codec verifies wire layout, the apply verifies the dispatcher
 * in client_sim_control.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "brain_list.h"
#include "transport_control_codec.h"
#include "transport_udp_internal.h"  /* PACKET_HEADER_SIZE */
#include "netpacks.h"                /* PACKET_LOBBY_* IDs */
#include "test_harness.h"

/* Encode `in` via the codec, then locate the decoder by the packet
 * type the encoder stamped into the header (byte 2 of the 8-byte
 * header) and decode the payload back into `out`. Returns 0 on
 * success, negative on failure. */
static int codec_roundtrip(ControlEventType type,
                           const ControlEvent *in,
                           ControlEvent *out) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(type);
    if (enc == NULL) return -1;
    EncodeResult r = enc(in, NULL, buf, sizeof(buf), &outLen);
    if (r != ENCODE_OK) return -2;
    if (outLen < PACKET_HEADER_SIZE) return -3;
    /* packHeader writes the packet type at byte 2. */
    uint8_t packetType = buf[2];
    ControlDecodeFn dec = transportControlCodecDecoder(packetType);
    if (dec == NULL) return -4;
    if (!dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, out)) {
        return -5;
    }
    return 0;
}

static ClientSim *fresh_client_sim(void) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs, gameOpen, false, 0, -1);
    clientSimSetPlayerNum(cs, 0);
    return cs;
}

/* ================================================================
 * CTRL_LOBBY_SETTINGS — extended with three trailing Layout A
 * fields. Round-trip every field; verify the apply path drops them
 * onto the matching ClientSim fields.
 * ================================================================ */
int run_lobby_settings_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    strncpy(in.u.lobbySettings.mapName, "TestMap",
            sizeof(in.u.lobbySettings.mapName) - 1);
    in.u.lobbySettings.lobbyGameType            = gameOpen;
    in.u.lobbySettings.lobbyHiddenMines         = true;
    in.u.lobbySettings.lobbyAiType              = 2;
    in.u.lobbySettings.lobbyTimeLimit           = 90000;
    in.u.lobbySettings.lobbyPillCount           = 11;
    in.u.lobbySettings.lobbyBaseCount           = 7;
    in.u.lobbySettings.lobbyStartCount          = 16;
    in.u.lobbySettings.mapSkipAvailable         = true;
    in.u.lobbySettings.netStat                  = netRunning;
    in.u.lobbySettings.inLobby                  = false;
    in.u.lobbySettings.lobbyOpenHost            = true;
    in.u.lobbySettings.lobbyAutoLockOnGameStart = true;
    in.u.lobbySettings.lobbyServerLocks         = 0xABCD;

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_SETTINGS, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_LOBBY_SETTINGS);
    UT_ASSERT(strcmp(out.u.lobbySettings.mapName, "TestMap") == 0);
    UT_ASSERT(out.u.lobbySettings.lobbyGameType            == gameOpen);
    UT_ASSERT(out.u.lobbySettings.lobbyHiddenMines         == true);
    UT_ASSERT(out.u.lobbySettings.lobbyAiType              == 2);
    UT_ASSERT(out.u.lobbySettings.lobbyTimeLimit           == 90000);
    UT_ASSERT(out.u.lobbySettings.lobbyPillCount           == 11);
    UT_ASSERT(out.u.lobbySettings.lobbyBaseCount           == 7);
    UT_ASSERT(out.u.lobbySettings.lobbyStartCount          == 16);
    UT_ASSERT(out.u.lobbySettings.mapSkipAvailable         == true);
    UT_ASSERT(out.u.lobbySettings.netStat                  == netRunning);
    UT_ASSERT(out.u.lobbySettings.inLobby                  == false);
    UT_ASSERT(out.u.lobbySettings.lobbyOpenHost            == true);
    UT_ASSERT(out.u.lobbySettings.lobbyAutoLockOnGameStart == true);
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyServerLocks == 0xABCD,
                  "serverLocks got 0x%04X want 0xABCD",
                  (unsigned)out.u.lobbySettings.lobbyServerLocks);

    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(strcmp(cs->mapName, "TestMap") == 0);
    UT_ASSERT(cs->lobbyGameType            == gameOpen);
    UT_ASSERT(cs->lobbyHiddenMines         == true);
    UT_ASSERT(cs->lobbyAiType              == 2);
    UT_ASSERT(cs->lobbyTimeLimit           == 90000);
    UT_ASSERT(cs->lobbyPillCount           == 11);
    UT_ASSERT(cs->lobbyBaseCount           == 7);
    UT_ASSERT(cs->lobbyStartCount          == 16);
    UT_ASSERT(cs->mapSkipAvailable         == true);
    UT_ASSERT(cs->netStat                  == netRunning);
    UT_ASSERT(cs->inLobby                  == false);
    UT_ASSERT(cs->lobbyOpenHost            == true);
    UT_ASSERT(cs->lobbyAutoLockOnGameStart == true);
    UT_ASSERT(cs->lobbyServerLocks         == 0xABCD);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_TEAM_META — `in_use` is not on the wire; the decoder
 * reconstructs it from (nameLen>0 || color!=0 || pool!=0). Two
 * sub-cases exercise both sides of that branch.
 * ================================================================ */
int run_lobby_team_meta_codec_and_apply(void) {
    /* Populated team: name + color + pool all set → in_use reconstructs to 1 */
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_TEAM_META;
    in.u.lobbyTeamMeta.teamId     = 3;
    in.u.lobbyTeamMeta.in_use     = 1;
    in.u.lobbyTeamMeta.color      = 5;
    in.u.lobbyTeamMeta.namingPool = 2;
    strncpy(in.u.lobbyTeamMeta.name, "Phoenix",
            sizeof(in.u.lobbyTeamMeta.name) - 1);

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_TEAM_META, &in, &out) == 0,
                  "codec_roundtrip failed (populated)");
    UT_ASSERT(out.type == CTRL_LOBBY_TEAM_META);
    UT_ASSERT(out.u.lobbyTeamMeta.teamId     == 3);
    UT_ASSERT(out.u.lobbyTeamMeta.in_use     == 1);
    UT_ASSERT(out.u.lobbyTeamMeta.color      == 5);
    UT_ASSERT(out.u.lobbyTeamMeta.namingPool == 2);
    UT_ASSERT(strcmp(out.u.lobbyTeamMeta.name, "Phoenix") == 0);

    /* Empty team: name="", color=0, pool=0 → in_use reconstructs to 0
     * even if the caller had set in_use=1 (it's recomputed). */
    ControlEvent empty_in, empty_out;
    memset(&empty_in, 0, sizeof(empty_in));
    empty_in.type = CTRL_LOBBY_TEAM_META;
    empty_in.u.lobbyTeamMeta.teamId = 4;
    empty_in.u.lobbyTeamMeta.in_use = 1; /* will be overwritten by decoder */
    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_TEAM_META,
                                  &empty_in, &empty_out) == 0,
                  "codec_roundtrip failed (empty)");
    UT_ASSERT(empty_out.u.lobbyTeamMeta.teamId == 4);
    UT_ASSERT_MSG(empty_out.u.lobbyTeamMeta.in_use == 0,
                  "decoder should clear in_use when name/color/pool are zero, got %u",
                  (unsigned)empty_out.u.lobbyTeamMeta.in_use);
    UT_ASSERT(empty_out.u.lobbyTeamMeta.name[0] == '\0');

    /* Apply: the populated event should land on cs->lobbyTeam* arrays. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(cs->lobbyTeamInUse[3] == 1);
    UT_ASSERT(cs->lobbyTeamColor[3] == 5);
    UT_ASSERT(cs->lobbyTeamPool[3]  == 2);
    UT_ASSERT(strcmp(cs->lobbyTeamName[3], "Phoenix") == 0);
    /* Untouched neighbour stays zero. */
    UT_ASSERT(cs->lobbyTeamInUse[4] == 0);
    UT_ASSERT(cs->lobbyTeamName[4][0] == '\0');
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BOT_CONFIG — the wire carries name but the apply path
 * intentionally ignores it (players.c is source of truth via the
 * lobbySlot path). Test both halves.
 * ================================================================ */
int run_lobby_bot_config_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_BOT_CONFIG;
    in.u.lobbyBotConfig.slot        = 5;
    in.u.lobbyBotConfig.difficulty  = 3;
    in.u.lobbyBotConfig.personality = 7;
    strncpy(in.u.lobbyBotConfig.name, "BotOnFive",
            sizeof(in.u.lobbyBotConfig.name) - 1);

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_CONFIG, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_LOBBY_BOT_CONFIG);
    UT_ASSERT(out.u.lobbyBotConfig.slot        == 5);
    UT_ASSERT(out.u.lobbyBotConfig.difficulty  == 3);
    UT_ASSERT(out.u.lobbyBotConfig.personality == 7);
    UT_ASSERT(strcmp(out.u.lobbyBotConfig.name, "BotOnFive") == 0);

    /* Apply: difficulty + personality land on the cs arrays. The name
     * field is informational on this event and must NOT stomp the
     * slot's playerName — the lobbySlot path owns that. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    /* Seed slot 5 with a known playerName so we can prove apply
     * didn't touch it. */
    strncpy(cs->lobbySlots[5].playerName, "ExistingSlotName",
            sizeof(cs->lobbySlots[5].playerName) - 1);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(cs->lobbyBotDifficulty[5]  == 3);
    UT_ASSERT(cs->lobbyBotPersonality[5] == 7);
    UT_ASSERT_MSG(
        strcmp(cs->lobbySlots[5].playerName, "ExistingSlotName") == 0,
        "lobbyBotConfig.name should not stomp lobbySlots[].playerName, got '%s'",
        cs->lobbySlots[5].playerName);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BOT_BRAIN — non-empty path round-trip and empty-path
 * sentinel (server fallback).
 * ================================================================ */
int run_lobby_bot_brain_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_BOT_BRAIN;
    in.u.lobbyBotBrain.slot = 2;
    strncpy(in.u.lobbyBotBrain.path, "Brains/NewAutopilot/init.lua",
            sizeof(in.u.lobbyBotBrain.path) - 1);

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_BRAIN, &in, &out) == 0,
                  "codec_roundtrip failed (non-empty path)");
    UT_ASSERT(out.type == CTRL_LOBBY_BOT_BRAIN);
    UT_ASSERT(out.u.lobbyBotBrain.slot == 2);
    UT_ASSERT(strcmp(out.u.lobbyBotBrain.path,
                     "Brains/NewAutopilot/init.lua") == 0);

    /* Empty-path sentinel — pathLen=0 on the wire, decoder leaves the
     * path as an empty string. */
    ControlEvent empty_in, empty_out;
    memset(&empty_in, 0, sizeof(empty_in));
    empty_in.type = CTRL_LOBBY_BOT_BRAIN;
    empty_in.u.lobbyBotBrain.slot = 6;
    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_BRAIN,
                                  &empty_in, &empty_out) == 0,
                  "codec_roundtrip failed (empty path)");
    UT_ASSERT(empty_out.u.lobbyBotBrain.slot == 6);
    UT_ASSERT(empty_out.u.lobbyBotBrain.path[0] == '\0');

    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(strcmp(cs->lobbyBotBrain[2],
                     "Brains/NewAutopilot/init.lua") == 0);
    /* Empty-path apply leaves the field empty (already zero, but
     * worth asserting we don't crash and don't poison the slot). */
    clientSimApplyControl(cs, &empty_in);
    UT_ASSERT(cs->lobbyBotBrain[6][0] == '\0');
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BRAIN_LIST — biggest payload. Tests a single-entry
 * list and a BRAIN_LIST_MAX-entry list to exercise the worst-case
 * encode path (which only fits because MAX_CONTROL_PACKET was bumped
 * to 8192 alongside this encoder).
 * ================================================================ */
int run_lobby_brain_list_codec_and_apply(void) {
    /* Single entry. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_LOBBY_BRAIN_LIST;
        in.u.lobbyBrainList.list.count = 1;
        strncpy(in.u.lobbyBrainList.list.entries[0].name,    "NewAutopilot",
                BRAIN_LIST_NAME_LEN - 1);
        strncpy(in.u.lobbyBrainList.list.entries[0].version, "2026-05-11 12:30",
                BRAIN_LIST_VER_LEN - 1);
        strncpy(in.u.lobbyBrainList.list.entries[0].path,
                "Brains/NewAutopilot/init.lua",
                BRAIN_LIST_PATH_LEN - 1);

        UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BRAIN_LIST, &in, &out) == 0,
                      "codec_roundtrip failed (single entry)");
        UT_ASSERT(out.type == CTRL_LOBBY_BRAIN_LIST);
        UT_ASSERT(out.u.lobbyBrainList.list.count == 1);
        UT_ASSERT(strcmp(out.u.lobbyBrainList.list.entries[0].name,
                         "NewAutopilot") == 0);
        UT_ASSERT(strcmp(out.u.lobbyBrainList.list.entries[0].version,
                         "2026-05-11 12:30") == 0);
        UT_ASSERT(strcmp(out.u.lobbyBrainList.list.entries[0].path,
                         "Brains/NewAutopilot/init.lua") == 0);
    }

    /* Full BRAIN_LIST_MAX entries — worst-case wire size, near the
     * 8192-byte cap. Each entry gets a unique name/version/path so
     * we can spot misordering. */
    ControlEvent full_in, full_out;
    memset(&full_in, 0, sizeof(full_in));
    full_in.type = CTRL_LOBBY_BRAIN_LIST;
    full_in.u.lobbyBrainList.list.count = BRAIN_LIST_MAX;
    int i;
    for (i = 0; i < BRAIN_LIST_MAX; i++) {
        snprintf(full_in.u.lobbyBrainList.list.entries[i].name,
                 BRAIN_LIST_NAME_LEN, "Brain%02d", i);
        snprintf(full_in.u.lobbyBrainList.list.entries[i].version,
                 BRAIN_LIST_VER_LEN, "v%d", i);
        snprintf(full_in.u.lobbyBrainList.list.entries[i].path,
                 BRAIN_LIST_PATH_LEN, "Brains/Brain%02d/init.lua", i);
    }

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BRAIN_LIST,
                                  &full_in, &full_out) == 0,
                  "codec_roundtrip failed (BRAIN_LIST_MAX entries)");
    UT_ASSERT(full_out.u.lobbyBrainList.list.count == BRAIN_LIST_MAX);
    for (i = 0; i < BRAIN_LIST_MAX; i++) {
        char wantName[BRAIN_LIST_NAME_LEN];
        char wantVer [BRAIN_LIST_VER_LEN];
        char wantPath[BRAIN_LIST_PATH_LEN];
        snprintf(wantName, sizeof(wantName), "Brain%02d", i);
        snprintf(wantVer,  sizeof(wantVer),  "v%d", i);
        snprintf(wantPath, sizeof(wantPath), "Brains/Brain%02d/init.lua", i);
        UT_ASSERT_MSG(strcmp(full_out.u.lobbyBrainList.list.entries[i].name,
                             wantName) == 0,
                      "entry %d name mismatch: got '%s' want '%s'",
                      i, full_out.u.lobbyBrainList.list.entries[i].name, wantName);
        UT_ASSERT(strcmp(full_out.u.lobbyBrainList.list.entries[i].version,
                         wantVer) == 0);
        UT_ASSERT(strcmp(full_out.u.lobbyBrainList.list.entries[i].path,
                         wantPath) == 0);
    }

    /* Apply: cs->lobbyBrainList is a straight struct copy. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &full_in);
    UT_ASSERT(cs->lobbyBrainList.count == BRAIN_LIST_MAX);
    UT_ASSERT(strcmp(cs->lobbyBrainList.entries[0].name,  "Brain00") == 0);
    UT_ASSERT(strcmp(cs->lobbyBrainList.entries[BRAIN_LIST_MAX - 1].name,
                     "Brain15") == 0);
    clientSimDestroy(cs);
    return 0;
}
